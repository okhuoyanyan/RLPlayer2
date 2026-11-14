// Product decisions, not DOM snapshots: unknown hardware never invents capabilities.
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../src/net/ui_script.h'), 'utf8');
const begin = source.indexOf('/* QUICK_START_LOGIC_BEGIN */');
const end = source.indexOf('/* QUICK_START_LOGIC_END */');
if (begin < 0 || end <= begin) throw new Error('Missing production quick-start decision/render logic');
const context = {t:s=>s, tf:(s,a)=>s.replace(/\{(\d+)\}/g,(_,i)=>a[i]),
  esc:s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))};
vm.createContext(context); vm.runInContext(source.slice(begin,end),context);
let checks=0;
function check(value,message){checks++;if(!value)throw new Error(message);}
let m=context.quickStartModel(null,'wasapi',{});
check(!m.known && !m.pcmReady && !m.dsdReady, 'unknown device cannot invent PCM/DSD support');
check(m.recommended==='', 'unknown capabilities do not recommend a high-rate preset');
const caps={ok:true,device:{name:'DAC <unsafe>',rates:[44100,48000,176400,192000],maxRate:192000,
  exclusive:true,dsdNative:false,dop:false}};
m=context.quickStartModel(caps,'wasapi',{gpuOn:true,gpuUsed:false});
check(m.pcmReady && !m.dsdReady && m.recommended==='pcm', 'known PCM-only DAC gets PCM recommendation');
check(m.gpuAvailable && !m.gpuUsed, 'available GPU is not claimed as currently used');
let html=context.quickStartHtml(m);
check(html.includes('DAC &lt;unsafe&gt;') && !html.includes('DAC <unsafe>'), 'device names are escaped');
check(html.includes('qsChooseOutput')&&html.includes('qsChooseMusic')&&html.includes('qsInspectSignal')&&html.includes('qsOpenEq'), 'four useful next-step controls exist');
check(html.includes('data-qs-profile="bitperfect"')&&html.includes('data-qs-profile="pcm"')&&html.includes('data-qs-profile="dsd"'), 'three primary choices exist');
m=context.quickStartModel(caps,'null',{});
check(!m.known&&!m.pcmReady&&!m.dsdReady&&m.recommended===''&&m.name!==caps.device.name,'offline Null must not borrow the default physical DAC capabilities');
m=context.quickStartModel(caps,'wasapi-shared',{});
check(m.shared && m.recommended==='', 'shared path never promises bit-perfect/high-rate hardware output');
m=context.quickStartModel(caps,'upnp',{});
check(!m.pcmReady && !m.dsdReady && m.recommended==='', 'local DAC caps must not be attributed to a remote renderer');
m=context.quickStartModel({ok:true,device:{...caps.device,dsdNative:true,dsdNativeMax:'dsd256'}},'asio',{});
check(m.dsdReady && m.dsdRoute==='Native DSD', 'ASIO native capability is distinct from DoP');
m=context.quickStartModel({ok:true,device:{...caps.device,dop:true,dopMax:'dsd128'}},'wasapi',{});
check(m.dsdReady && m.dsdRoute==='DoP', 'DoP is explicitly identified');
check(!/\/api\/play|autoplay/.test(context.quickStartHtml(m)), 'rendering contains no automatic playback action');
console.log('quick_start: '+checks+' checks PASS');
