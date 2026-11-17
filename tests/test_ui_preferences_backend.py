"""Exercise the real file-backed settings handler; no audio/server/user profile."""
from pathlib import Path
import argparse, ast, hashlib, json, os, subprocess, tempfile

root=Path(os.environ.get('RLPLAYER_TEST_SOURCE_ROOT',Path(__file__).resolve().parents[1]))
p=argparse.ArgumentParser()
p.add_argument('--compiler',default=r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe')
p.add_argument('--evidence-dir',type=Path)
a=p.parse_args()
# Reuse only literal test scaffolding, never execute another test at import time.
tree=ast.parse((root/'tests/test_app_settings_persistence.py').read_text('utf-8'))
constants={n.targets[0].id:ast.literal_eval(n.value) for n in tree.body
           if isinstance(n,ast.Assign) and len(n.targets)==1 and isinstance(n.targets[0],ast.Name)
           and n.targets[0].id in ('prefix','wrapper')}
prefix=constants['prefix']
source=(root/'tools/rtaweb.cpp').read_text('utf-8')
start=source.index('    if (rq.path.rfind("/api/settings", 0) == 0) {')
handler=source[start:source.index('    // ASIO 驱动列表',start)]
store=(root/'src/meta/store.h').read_text('utf-8')
atomic=store[store.index('inline std::recursive_mutex& meta_file_mutex('):store.index('\nclass MetaStore')]
workbench=(root/'src/net/workbench_config.h').read_text('utf-8')
error_start=workbench.index('inline std::string error(')
workbench_error=workbench[error_start:workbench.index('\n}',error_start)+2]
def production_function(text,marker):
    start=text.index(marker)
    return text[start:text.index('\n}',start)+2]
form=production_function(workbench,'inline std::string form(')
params_start=source.index('struct PlayParams {')
params=source[params_start:source.index('\n};',params_start)+3]
helpers='\n'.join(production_function(source,marker) for marker in (
    'static std::string jesc(', 'static std::string urlDecode(',
    'static std::string artistimg_pct(', 'static rta::dsp::Shaper parse_shaper(',
    'static std::string saved_backend_setting()', 'static double saved_volume_setting()',
    'static bool saved_roomcorr_auto()', 'static std::string saved_rate_family_policy()',
    'static PlayParams parseForm(', 'static std::string pcm_shaper_request_error('))
main=r'''
return "unhandled";}
int main(int argc,char**argv){base=argv[1];int failures=0,checks=0;
auto check=[&](bool b,const char* what){++checks;if(!b)++failures;printf("%s %s\n",b?"PASS":"FAIL",what);};
auto read=[](const std::string&p){std::ifstream f(p);return std::string(std::istreambuf_iterator<char>(f),{});};
auto has=[](const std::string&j,const char*k,const char*v){return j.find(std::string("\"")+k+"\":\""+v+"\"")!=std::string::npos;};
const auto file=path_in_exe_dir("rlplayer-settings.conf");
if(argc>2){auto j=settings({"/api/settings","GET",""});
check(has(j,"quickstart","0"),"disabled quick start survives a new process");
check(has(j,"cleanup_auto","1"),"automatic cleanup mode survives a new process");
check(has(j,"backend","null")&&has(j,"volume","-60"),"audio settings unchanged across new process");
const int originalChecks=checks;
check(workbench::syncCalls==0,"new-process readback does not write the profile bank");
check(has(j,"bits","32")&&has(j,"dacbits","20")&&has(j,"shaper","NS9"),"container, DAC precision and manual shaper survive a new process independently");
printf("original_checks=%d added_workbench_checks=%d sync_calls=%d\n",originalChecks,checks-originalChecks,workbench::syncCalls.load());
printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
auto j=settings({"/api/settings","GET",""});
const bool initialReadOnly=workbench::syncCalls==0;
check(has(j,"quickstart","1"),"quick start defaults on without saved key");
check(has(j,"cleanup_auto","1"),"cleanup noise estimate defaults automatic");
std::ofstream(file)<<"backend=null\nvolume=-60\nprivate_setting=keep\n";
auto post=[&](const std::string&b){rta::net::lastStatus=200;return settings({"/api/settings","POST",b});};
check(post("quickstart=0&cleanup_auto=0&cleanup_noise_db=-99").find("\"ok\":true")!=std::string::npos,"persist independent quick start and manual cleanup preferences");
const auto firstSyncCalls=workbench::syncCalls.load();const auto firstSyncBytes=workbench::lastSavedBytes;
j=settings({"/api/settings","GET",""});
check(has(j,"quickstart","0")&&has(j,"cleanup_auto","0"),"read back saved off/manual values");
check(read(file).find("private_setting=keep")!=std::string::npos,"unrelated settings preserved");
post("theme=dark");j=settings({"/api/settings","GET",""});
check(has(j,"quickstart","0")&&has(j,"cleanup_auto","0"),"partial unrelated save preserves preferences");
const auto before=read(file);
const auto beforeInvalid=workbench::syncCalls.load();
auto bad=post("quickstart=2");
check(rta::net::lastStatus==400&&bad.find("\"ok\":false")!=std::string::npos,"reject invalid quick start preference");
check(read(file)==before,"invalid quick start does not alter any settings bytes");
bad=post("cleanup_auto=on");
check(rta::net::lastStatus==400&&bad.find("\"ok\":false")!=std::string::npos,"reject invalid noise estimate mode");
check(read(file)==before,"invalid cleanup mode does not alter settings bytes");
const auto afterInvalid=workbench::syncCalls.load();
post("quickstart=true&cleanup_auto=true");j=settings({"/api/settings","GET",""});
const auto aliasSyncCalls=workbench::syncCalls.load();const auto aliasSyncBytes=workbench::lastSavedBytes;
check(has(j,"quickstart","1")&&has(j,"cleanup_auto","1"),"accepted boolean aliases are normalized on disk");
check(has(j,"cleanup_noise_db","-99"),"manual noise floor retained while auto mode selected");
post("quickstart=0");
const int originalChecks=checks;
check(initialReadOnly,"default preference GET does not synchronize the profile bank");
check(firstSyncCalls==1&&firstSyncBytes.find("quickstart=0")!=std::string::npos&&firstSyncBytes.find("cleanup_auto=0")!=std::string::npos,"profile collaborator receives the committed independent preferences");
check(afterInvalid==beforeInvalid,"invalid preferences never call profile synchronization");
check(aliasSyncCalls==afterInvalid+1&&aliasSyncBytes.find("quickstart=1")!=std::string::npos&&aliasSyncBytes.find("cleanup_auto=1")!=std::string::npos,"boolean aliases synchronize once after canonical values are persisted");
const auto beforeFailure=workbench::syncCalls.load();workbench::syncSuccess=false;
const auto failedSync=post("quickstart=1&cleanup_auto=0");const int failedStatus=rta::net::lastStatus;
check(workbench::syncCalls==beforeFailure+1,"preference write invokes the injected profile failure once");
check(failedStatus==500&&failedSync.find("\"ok\":false")!=std::string::npos,"real preference handler returns HTTP 500 for profile persistence failure");
check(read(file).find("quickstart=1")!=std::string::npos&&read(file).find("cleanup_auto=0")!=std::string::npos&&read(file).find("private_setting=keep")!=std::string::npos,"profile failure reports already saved preferences without losing unrelated settings");
workbench::syncSuccess=true;const auto recovered=post("quickstart=0&cleanup_auto=1");
check(rta::net::lastStatus==200&&recovered.find("\"ok\":true")!=std::string::npos&&workbench::syncCalls==beforeFailure+2,"preference synchronization recovers and retains the new-process baseline");
check(post("bits=32&dacbits=20&shaper=NS9&dsd=0").find("\"ok\":true")!=std::string::npos,"valid audio precision can be saved alongside UI preferences");
const auto beforePrecision=read(file);const auto syncBeforePrecision=workbench::syncCalls.load();
const auto invalidPrecision=post("dacbits=8&quickstart=1");
check(rta::net::lastStatus==400&&invalidPrecision.find("16 bit")!=std::string::npos,"partial UI save rejects a retained manual shaper with unsupported effective precision");
check(read(file)==beforePrecision&&workbench::syncCalls==syncBeforePrecision,"invalid precision does not partly persist UI changes or synchronize the profile");
j=settings({"/api/settings","GET",""});
check(has(j,"quickstart","0")&&has(j,"cleanup_auto","1")&&has(j,"dacbits","20"),"rejected audio change preserves independent UI and cleanup preferences");
printf("original_checks=%d added_workbench_checks=%d sync_calls=%d sync_failure_http_status=%d\n",originalChecks,checks-originalChecks,workbench::syncCalls.load(),failedStatus);
printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
'''
with tempfile.TemporaryDirectory(prefix='rlplayer-ui-preferences-') as t:
    t=Path(t);cpp=t/'test.cpp';exe=t/'test.exe';profile=t/'profile';profile.mkdir()
    fixture=prefix+atomic+constants['wrapper'].replace('// PRODUCTION_PLAY_PARAMS',params).replace('// PRODUCTION_SETTINGS_HELPERS',helpers).replace('// PRODUCTION_WORKBENCH_FORM',form).replace('// PRODUCTION_WORKBENCH_ERROR',workbench_error)+handler+main
    cpp.write_text(fixture,encoding='utf-8')
    subprocess.run([a.compiler,'-std=c++17','-O0','-I'+str(root/'src'),str(cpp),'-o',str(exe)],check=True)
    first=subprocess.run([str(exe),str(profile)],capture_output=True,text=True,encoding='utf-8',errors='replace')
    second=subprocess.run([str(exe),str(profile),'read-only-new-process'],capture_output=True,text=True,encoding='utf-8',errors='replace')
    result={'firstExit':first.returncode,'newProcessExit':second.returncode,'stdout':first.stdout+second.stdout,'stderr':first.stderr+second.stderr,'audioOpened':False,'serverStarted':False,'productionHandlerExtracted':True,'productionErrorExtracted':True,
            'profileCollaborator':'isolated fault-injectable spy','productionSourceSHA256':hashlib.sha256((root/'tools/rtaweb.cpp').read_bytes()).hexdigest(),
            'productionHandlerSHA256':hashlib.sha256(handler.encode()).hexdigest(),'productionErrorSHA256':hashlib.sha256(workbench_error.encode()).hexdigest(),
            'productionFormExtracted':True,'productionFormSHA256':hashlib.sha256(form.encode()).hexdigest(),
            'productionParsingAndPrecisionHelpersSHA256':hashlib.sha256(helpers.encode()).hexdigest(),
            'fixtureSHA256':hashlib.sha256(fixture.encode()).hexdigest(),'executedEXESHA256':hashlib.sha256(exe.read_bytes()).hexdigest()}
    print(result['stdout'],end='')
    if a.evidence_dir:
        a.evidence_dir.mkdir(parents=True,exist_ok=True)
        (a.evidence_dir/'fixture.cpp').write_text(fixture,encoding='utf-8')
        (a.evidence_dir/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    raise SystemExit(1 if first.returncode or second.returncode else 0)
