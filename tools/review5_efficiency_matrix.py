"""Serial offline production-route performance, with complete frame/hash gates.

Source duration / full harness wall is the conservative primary speed. The
harness includes validation/hash work, excludes PCM fixture generation/loading
and GUI ISP/device handling; it is not electrical energy or GUI first sound.
"""
import argparse,datetime,hashlib,json,os,pathlib,platform,statistics,subprocess,time
import psutil

def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def cpu_snapshot():
    result={}
    for p in psutil.process_iter(['name','cpu_times']):
        if p.pid==0:continue  # Windows Idle is not competing background work.
        try:result[p.pid]=(p.info['name'],p.info['cpu_times'].user+p.info['cpu_times'].system)
        except (psutil.NoSuchProcess,psutil.AccessDenied,TypeError):pass
    return result

def verify_execution(result, gpu_requested):
    """Match the production integer-first-stage policy, without enabling GPU
    on a stage that deliberately uses exact integer accumulation. Any missing
    dispatch on an eligible floating stage still fails this benchmark.
    """
    fixed=result.get('actual_fixed_all') is True
    if fixed != (result.get('actual_fixed_any') is True):
        raise RuntimeError('Fixed-stage selection changed between blocks')
    cascade=result.get('cascade_active') is True
    eligible=(2 if fixed else 3) if cascade else (0 if fixed else 1)
    expected=eligible if gpu_requested else 0
    if gpu_requested and not expected:
        raise RuntimeError('This benchmark case has no GPU-eligible stage')
    if result.get('gpu_mask_or')!=expected or result.get('gpu_mask_and')!=expected:
        raise RuntimeError(f'Actual GPU masks do not match all eligible stages: expected {expected}')
    if (result.get('actual_gpu_any') is True)!=(expected!=0):
        raise RuntimeError('GPU execution flag contradicts dispatch masks')
    all_stages=expected==(3 if cascade else 1)
    if (result.get('actual_gpu_all_blocks_all_stages') is True)!=all_stages:
        raise RuntimeError('GPU all-stages flag contradicts the production route')
    return dict(expected_gpu_stage_mask=expected,integer_first_stage=fixed,
                gpu_scope='eligible floating stages; integer first stage remains on CPU' if gpu_requested else 'CPU only')

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe',type=pathlib.Path,required=True)
    ap.add_argument('--output',type=pathlib.Path,required=True)
    ap.add_argument('--seconds',type=int,default=30,choices=[10,30,60])
    ap.add_argument('--run',action='store_true');ap.add_argument('--case',action='append')
    args=ap.parse_args();root=args.output.resolve();root.mkdir(parents=True,exist_ok=True)
    cases=[]
    def add(name,route,source,target,gpu=0,preset='poly-sinc-short-lp',family='dsd',order=5):
        cases.append(dict(id=name,route=route,source=source,target=target,gpu=gpu,preset=preset,family=family,order=order))
    for gpu in (0,1):
        suffix='gpu' if gpu else 'cpu'
        add('pcm705_'+suffix,'pcm_pcm',44100,705600,gpu)
        add('pcm1536_'+suffix,'pcm_pcm',48000,1536000,gpu)
        add('gaussxl384_'+suffix,'pcm_pcm',44100,384000,gpu,'poly-sinc-gauss-xl')
        add('pcm_dsd1024_'+suffix,'pcm_dsd',44100,45158400,gpu)
        add('dsd64_remod1024_'+suffix,'dsd_decode_remod',2822400,45158400,gpu)
    add('pcm_dsd512_gpu','pcm_dsd',44100,22579200,1)
    add('pcm_dsd1024_ec7_gpu','pcm_dsd',44100,45158400,1,family='ec',order=7)
    add('dsd64_native1024_ec7_cpu','dsd_native_convert',2822400,45158400,family='ec',order=7)
    add('dsd64_direct','dsd_direct',2822400,2822400)
    add('long_mp_crossfamily','pcm_pcm',44100,48000,preset='poly-sinc-long-mp')
    if args.case:
        unknown=set(args.case)-{c['id'] for c in cases}
        if unknown:raise ValueError(f'Unknown cases: {unknown}')
        cases=[c for c in cases if c['id'] in args.case]
    receipt=dict(schema=1,status='PLANNED',duration=args.seconds,trials=3,cases=cases,
                 scope=__doc__,device_opened=False,created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    (root/'plan.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf-8')
    if not args.run:print(json.dumps(receipt,ensure_ascii=False));return 0
    if (root/'summary.json').exists():raise ValueError('Use a new output directory; immutable prior run exists')
    exe=args.exe.resolve();receipt['exe_sha256']=sha(exe)
    receipt['driver_sha256']=sha(pathlib.Path(__file__).resolve())
    receipt['host']=dict(os=platform.platform(),logical_cpus=psutil.cpu_count(),memory_bytes=psutil.virtual_memory().total)
    receipt['dll_sha256']={p.name:sha(p) for p in exe.parent.glob('*.dll')}
    env=os.environ.copy();env['RLPLAYER_FILTER_CACHE']='1'
    fixture_root=root/'fixtures';fixture_root.mkdir(exist_ok=True)
    all_rows=[]
    # Construct/validate the shared generated DSD fixture before any measured
    # case. Its samples are still hashed and its bit-count verified on each run.
    if any(c['route'].startswith('dsd_') for c in cases):
        fixture=fixture_root/f'dsd64-{args.seconds}s.dsf'
        prep=subprocess.run([str(exe),'--route','dsd_direct','--source-rate','2822400','--target-rate','2822400',
                             '--seconds',str(args.seconds),'--fixture',str(fixture)],cwd=exe.parent,env=env,capture_output=True,timeout=300)
        (root/'fixture-preparation.stdout.json').write_bytes(prep.stdout)
        (root/'fixture-preparation.stderr.log').write_bytes(prep.stderr)
        if prep.returncode:raise RuntimeError('Fixture preparation failed')
    for trial in range(1,4):
        for c in (cases if trial%2 else list(reversed(cases))):
            path=root/f"{c['id']}-{trial}";path.mkdir()
            env['RLPLAYER_FILTER_CACHE_DIR']=str(root/'cache'/c['id'])
            flags=['--route',c['route'],'--source-rate',str(c['source']),'--target-rate',str(c['target']),
                   '--seconds',str(args.seconds),'--gpu',str(c['gpu']),'--preset',c['preset'],
                   '--family',c['family'],'--order',str(c['order']),'--block','4096','--topology','auto']
            if c['route'].startswith('dsd_'):
                flags+=['--fixture',str(fixture_root/f'dsd64-{args.seconds}s.dsf'),'--decode-mode','stream']
            if c['route'] in ('pcm_dsd','dsd_decode_remod'):flags+=['--overlap','1']
            idle=psutil.cpu_percent(interval=1)
            bg=cpu_snapshot();psutil.cpu_percent(interval=None);start=time.monotonic()
            run=subprocess.run([str(exe),*flags],cwd=exe.parent,env=env,capture_output=True,timeout=300)
            wall=time.monotonic()-start;machine_busy=psutil.cpu_percent(interval=None);after=cpu_snapshot()
            (path/'stdout.json').write_bytes(run.stdout);(path/'stderr.log').write_bytes(run.stderr)
            if run.returncode:raise RuntimeError(f"{c['id']} trial {trial} exit {run.returncode}")
            result=json.loads(run.stdout)
            for field in ('structural_ok','count_ok','full_output_checksum'):
                if result.get(field) is not True:raise RuntimeError(f'{c["id"]}: {field} failed')
            for field in ('marker_errors','finite_errors','integer_24bit_range_errors','sdm_nonfinite','sdm_overload','sdm_clamped'):
                if result.get(field)!=0:raise RuntimeError(f'{c["id"]}: {field} nonzero')
            execution=verify_execution(result,c['gpu'])
            external=[]
            for pid,(name,cpu) in after.items():
                if pid in bg and pid!=os.getpid() and cpu>bg[pid][1]:
                    external.append(dict(pid=pid,name=name,machine_percent=100*(cpu-bg[pid][1])/wall/psutil.cpu_count()))
            external.sort(key=lambda p:-p['machine_percent'])
            row=dict(case=c['id'],trial=trial,pre_run_machine_busy_percent=idle,machine_busy_percent_during=machine_busy,
                     logical_cpus=psutil.cpu_count(),external_cpu=external[:10],execution=execution,
                     process_wall_seconds=wall,result=result)
            all_rows.append(row);(path/'observation.json').write_text(json.dumps(row,indent=2),encoding='utf-8')
            (root/'trials.json').write_text(json.dumps(all_rows,indent=2),encoding='utf-8')
            print(c['id'],trial,'speed',result['end_to_end_realtime_factor'],'cpu/audio',result['harness_cpu_seconds_per_audio_second'],flush=True)
    summary=[]
    for c in cases:
        rows=[r for r in all_rows if r['case']==c['id']]
        hashes={r['result']['full_output_sha256'] for r in rows}
        if len(hashes)!=1:raise RuntimeError(f'{c["id"]}: repeated complete outputs differ')
        record=dict(case=c,complete_trials=len(rows),full_output_sha256=next(iter(hashes)))
        fields=('end_to_end_realtime_factor','harness_cpu_seconds_per_audio_second','harness_wall_s','steady_wall_s',
                'first_pipeline_prepare_ms','first_block_wall_s','eof_wall_s','peak_working_set_bytes',
                'input_frames','input_bits','output_frames','head_trimmed','drained','expected_output_frames')
        record['median']={k:statistics.median(r['result'][k] for r in rows) for k in fields}
        record['speed_min']=min(r['result']['end_to_end_realtime_factor'] for r in rows)
        record['speed_max']=max(r['result']['end_to_end_realtime_factor'] for r in rows)
        record['cold_prepare_ms']=rows[0]['result']['first_pipeline_prepare_ms']
        record['warm_prepare_ms']=[r['result']['first_pipeline_prepare_ms'] for r in rows[1:]]
        summary.append(record)
    comparisons=[]
    for name in ('pcm705','pcm1536','gaussxl384','pcm_dsd1024','dsd64_remod1024'):
        pair=[r for r in summary if r['case']['id'] in (name+'_cpu',name+'_gpu')]
        if len(pair)==2:
            equal=pair[0]['full_output_sha256']==pair[1]['full_output_sha256']
            comparisons.append(dict(case=name,cpu_gpu_full_sha256_equal=equal))
            if not equal:raise RuntimeError(f'{name}: CPU/GPU complete output SHA256 differs')
    receipt.update(status='PASS',rows=summary,cpu_gpu_comparisons=comparisons,
                   finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    (root/'summary.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf-8')
    print('PASS',len(summary),'cases',len(all_rows),'runs');return 0
if __name__=='__main__':raise SystemExit(main())
