const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=process.env.RLPLAYER_TEST_SOURCE_ROOT||path.join(__dirname,'..');
const source=fs.readFileSync(path.join(root,'src/net/ui_script.h'),'utf8');
function fn(n){const a=source.indexOf('function '+n+'(');return a<0?'':source.slice(a,source.indexOf('\n}',a)+2);}
const budget=process.argv[2]?fs.readFileSync(process.argv[2],'utf8').trim():'Requested FIR exceeds the coefficient budget; quality was not reduced. inRate=44100 outRate=384000 L=1280 M=147 T=32767 taps=41941760 budget=16777216. Use a cascade or explicitly change the filter configuration.';
const budgetKey='FIR 设计超出系数预算，已拒绝且未降低音质：{0} → {1} Hz，请求 {2} 个系数，预算 {3} 个。请启用两段式升频，或调整滤波器/输出率。';
const invalidKey='滤波器参数无效，已拒绝处理。请检查采样率、声道数、相位、衰减、过渡带及抽头设置。';
const rangeKey='滤波器配置超出支持范围，已拒绝处理且未降低音质。请使用两段式升频或调整滤波器/输出率。';
const cancelKey='滤波器准备已取消，未开始输出。';
let checks=0,failures=0;
function check(ok,label){++checks;if(!ok)++failures;console.log((ok?'PASS ':'FAIL ')+label);}
for(const lang of ['en','ja','fr','ru','es','ar']){
 const map=JSON.parse(fs.readFileSync(path.join(root,'data/i18n/'+lang+'.json'),'utf8'));
 const c={I18N:{map}};vm.createContext(c);for(const n of ['i18nNorm','i18nGet','t','tf'])vm.runInContext(fn(n),c);
 const helper=fn('playbackErrorText');if(helper)vm.runInContext(helper,c);else c.playbackErrorText=s=>s;
 const expected=(map[budgetKey]||'__MISSING__').replace('{0}','44100').replace('{1}','384000').replace('{2}','41941760').replace('{3}','16777216');
 check(c.playbackErrorText(budget)===expected,lang+' exact FIR budget refusal has localized guidance and exact rate/count values');
 check(c.playbackErrorText('渲染失败: '+budget)===(map['渲染失败：{0}']||'__MISSING__').replace('{0}',expected),lang+' render failure prefix and budget body both localize');
 for(const text of ['Resampler sample rates must be positive','Resampler supports 1 to 64 channels','Resampler transition must be finite and in [0,1)','Invalid resampler phase mix'])
  check(c.playbackErrorText(text)===(map[invalidKey]||'__MISSING__'),lang+' known invalid config class localizes');
 for(const text of ['Resampler coefficient count overflow','Cascade intermediate sample rate overflow','Automatic source support exceeds the tap integer range'])
  check(c.playbackErrorText(text)===(map[rangeKey]||'__MISSING__'),lang+' known representability/overflow class localizes');
 check(c.playbackErrorText('Filter preparation cancelled')===(map[cancelKey]||'__MISSING__'),lang+' known cancellation class localizes');
 check(c.playbackErrorText('Manual FIR is invalid; playback was not started.')===(map['手动 IR 无效，已拒绝播放。请检查文件、采样率、声道数和 FIR 数据。']||'__MISSING__'),lang+' actual rejected manual IR has localized recoverable guidance');
 check(c.playbackErrorText('D:/Manual FIR is invalid; playback was not started.wav')==='D:/Manual FIR is invalid; playback was not started.wav',lang+' manual IR class does not translate arbitrary path strings');
 for(const text of ['扬声器 (D18) device busy','D:\\测试\\Requested FIR exceeds the coefficient budget.wav','Requested FIR exceeds the coefficient budget; device path /raw/name','User error: Filter preparation cancelled'])
  check(c.playbackErrorText(text)===text,lang+' arbitrary endpoint/path/unrecognized error remains verbatim');
 check(c.playbackErrorText('渲染失败: 扬声器 (D18) device busy')===(map['渲染失败：{0}']||'__MISSING__').replace('{0}','扬声器 (D18) device busy'),lang+' only fixed render prefix changes; device name and raw reason survive');
}
console.log(JSON.stringify({checks,failures}));process.exitCode=failures?1:0;
