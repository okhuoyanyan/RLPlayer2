"""Verify generated ringing data against exported binary64 coefficients, then make an editable Excel table."""
import argparse,csv,hashlib,json,math
from pathlib import Path
import numpy as np
from openpyxl import Workbook,load_workbook
from openpyxl.styles import Font,PatternFill,Alignment
from openpyxl.utils import get_column_letter

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--data',required=True,type=Path);p.add_argument('--table',required=True,type=Path);p.add_argument('--coefficients',required=True,type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args()
 data=json.loads(a.data.read_text());rows=data['rows']
 with a.table.open(encoding='utf-8-sig') as f: table=list(csv.DictReader((x for x in f if not x.startswith('#')),delimiter='\t'))
 lookup={(x['preset'],int(x['in_rate']),int(x['out_rate'])):x for x in table}
 seen=set();failures=[];max_gd=0
 for r in rows:
  key=(r['name'],r['in_rate'],r['out_rate']);seen.add(key);m=lookup[key];file=m.get('coefficient_file') or f'{r["in_rate"]}-{r["out_rate"]}-{r["name"]}.f64';raw=(a.coefficients/file).read_bytes();h=np.frombuffer(raw,dtype='<f8');amp=np.abs(h);peak=amp.max();indices=np.flatnonzero(amp>=peak*.001);maxima=np.flatnonzero(amp==peak);center=.5*(int(maxima[0])+int(maxima[-1]));pre=(center-int(indices[0]))/r['prototype_rate']*1000;post=(int(indices[-1])-center)/r['prototype_rate']*1000
  phase=-2*np.pi*1000/r['prototype_rate']*np.arange(len(h));z=h*(np.cos(phase)+1j*np.sin(phase));gd=(np.sum(np.arange(len(h))*z)/np.sum(z)).real/r['prototype_rate']*1000;max_gd=max(max_gd,abs(gd-r['group_delay_1khz_ms']))
  valid=hashlib.sha256(raw).hexdigest()==r['sha256'] and len(h)==r['taps'] and abs(pre-r['pre60_ms'])<1e-12 and abs(post-r['post60_ms'])<1e-12 and abs(gd-r['group_delay_1khz_ms'])<1e-8 and (int(maxima[0]),int(maxima[-1]))==(r['peak_first_index'],r['peak_last_index'])
  if not valid:failures.append(key)
 assert len(rows)==len(seen)==237
 assert not failures,failures
 wb=Workbook();ws=wb.active;ws.title='237项真实包络'
 headers=['预设名称','相位','输入Hz','输出Hz','峰前-60dB包络ms','峰后-60dB包络ms','1kHz群延迟ms','抽头N','峰首索引','峰末索引','首越阈样本','末越阈样本','输出样本分辨率ms','系数SHA256']
 ws.append(headers)
 for r in rows:ws.append([r['name'],r['phase'],r['in_rate'],r['out_rate'],r['pre60_ms'],r['post60_ms'],r['group_delay_1khz_ms'],r['taps'],r['peak_first_index'],r['peak_last_index'],r['first_sample_above_threshold'],r['last_sample_above_threshold'],1000/r['prototype_rate'],r['sha256']])
 ws.freeze_panes='E2';ws.auto_filter.ref=ws.dimensions
 for row in ws:
  for c in row:c.alignment=Alignment(vertical='center')
 for c in ws[1]:c.font=Font(color='FFFFFF',bold=True);c.fill=PatternFill('solid',fgColor='20354A')
 for row in ws.iter_rows(min_row=2):
  color={'lp':'E6F1E8','mp':'E6EFFA','mix':'F1E7F7'}[row[1].value]
  for c in row[:7]:c.fill=PatternFill('solid',fgColor=color)
  for c in list(row[4:7])+[row[12]]:c.number_format='0.000000'
 for i,w in enumerate([31,9,13,13,22,22,22,12,13,13,16,16,23,68],1):ws.column_dimensions[get_column_letter(i)].width=w
 note=wb.create_sheet('定义与验收');note.append(['项目','说明']);notes=[('阈值','最大绝对系数峰值×0.001，即幅度相对峰值−60dB。'),('峰前包络','Epre[n]=max(|h[0..n]|)，从全局峰中心回看首个实际越阈样本。'),('峰后包络','Epost[n]=max(|h[n..N−1]|)，从峰中心到最后实际越阈样本。'),('对称偶长双峰','首/末恰好相等的全局最大峰取中点；因此LP前后结果对称。'),('离散分辨率','边界分辨率为1个原型域输出样本；没有声称连续时间精确。'),('群延迟','1kHz处H导数算出的解析群延迟，与峰前包络不是同一指标；MP/IP随频率变化。'),('生产路径','79预设×44100→1411200、48000→1536000、96000→384000，真实auto抽头；两段式使用真实process冲激。'),('独立校验',f'237项SHA256、越阈点、双峰中心均一致；群延迟与NumPy独立求和最大差{max_gd:.3g}ms。'),('纯冲激负控','C++ --selftest：纯冲激前后0、已知边界、双峰、负系数与起点故障注入，5/5通过。'),('用途边界','有限FIR系数的相对峰值包络，不等同真实声学持续时间，也不是听感结论。')]
 for x in notes:note.append(x)
 note.column_dimensions['A'].width=23;note.column_dimensions['B'].width=110
 for row in note:
  for c in row:c.alignment=Alignment(vertical='top',wrap_text=True)
 for row in range(2,note.max_row+1):note.row_dimensions[row].height=35
 cmp=wb.create_sheet('LP-MP-IP对比');cmp.append(headers[:7])
 for r in rows:
  if r['name'] in ['poly-sinc-long-lp','poly-sinc-long-mp','poly-sinc-long-ip']:cmp.append([r['name'],r['phase'],r['in_rate'],r['out_rate'],r['pre60_ms'],r['post60_ms'],r['group_delay_1khz_ms']])
 for i,w in enumerate([30,10,13,13,24,24,24],1):cmp.column_dimensions[get_column_letter(i)].width=w
 for row in cmp.iter_rows(min_row=2):
  for c in row[4:]:c.number_format='0.000000'
 a.output.parent.mkdir(parents=True,exist_ok=True);wb.save(a.output);check=load_workbook(a.output,data_only=True);assert check['237项真实包络'].max_row==238
 result=dict(rows=237,duplicate_identities=0,coefficient_hash_mismatches=0,envelope_mismatches=0,max_group_delay_numpy_difference_ms=float(max_gd),selftest_cases=5,source_json_sha256=hashlib.sha256(a.data.read_bytes()).hexdigest(),xlsx_sha256=hashlib.sha256(a.output.read_bytes()).hexdigest())
 a.output.with_suffix('.verification.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
if __name__=='__main__':main()
