"use strict";
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
const root=process.env.RLPLAYER_TEST_SOURCE_ROOT||path.resolve(__dirname,'..');
const source=fs.readFileSync(path.join(root,'src/net/ui_script.h'),'utf8');
function fn(name){const at=source.search(new RegExp('^function '+name+'\\(', 'm'));if(at<0)throw Error(name);return source.slice(at,source.indexOf('\n}',at)+2);}
const server=fs.readFileSync(path.join(root,'tools/rtaweb.cpp'),'utf8');
const section=server.slice(server.indexOf('struct Entry { const char* id; const char* description; }'),server.indexOf('struct Entry { const char* id; const char* description; }')+2400);
const options=Array.from(section.matchAll(/\{"(auto|none|NS1|NS4|NS5|NS9|LNS15|RPDF|TPDF|Gauss1|shaped)", "([^"]+)"\}/g),m=>({id:m[1],description:m[2],recommended:true}));
let checks=0,failed=0;const check=(ok,s)=>{checks++;if(!ok)failed++;console.log((ok?'PASS ':'FAIL ')+s);};
(async()=>{
 const core=source.match(/return R"JSEOF\(([\s\S]*?)\)JSEOF";/);new vm.Script(core[1]);
 const studio=fs.readFileSync(path.join(root,'src/net/ui_studio.h'),'utf8').match(/rta_ui_studio_js\(\)\{return R"STUDIO\(([\s\S]*?)\)STUDIO";/);new vm.Script(studio[1]);
 check(true,'production core and studio JavaScript parse after control integration');
 check(options.length===11,'guide covers all server-defined options, including automatic');
 check(options.some(o=>o.id==='LNS15'&&o.description.includes('超声噪声较多，建议手动比较。')),'LNS15 explicitly explains its ultrasonic noise tradeoff and manual comparison');
 for(const lang of ['en','ja','fr','ru','es','ar']){
  const dict=JSON.parse(fs.readFileSync(path.join(root,'data/i18n',lang+'.json'),'utf8'));
  check(options.every(o=>typeof dict[o.description]==='string'&&dict[o.description]!==o.description),lang+' translates every live backend option description');
  const nodes={sShap:{value:'auto'},sDSD:{value:'0'}};
  const c={Promise,Number,String,S:{view:'albums'},D:id=>nodes[id]||null,rlLastStatus:{playing:false,outRate:192000,outBits:24},t:s=>dict[s]||s,tf:(s,a)=>(dict[s]||s).replace(/\{(\d+)\}/g,(_,i)=>a[i]),RL_SHAPER_INFO:{cache:{},pending:{}}};
  vm.createContext(c);for(const name of ['rlShaperConditions','rlShaperSummary','rlLoadShaperInfo'])vm.runInContext(fn(name),c);
  let state=c.rlShaperConditions(),metadata={ok:true,options,autoResolved:'NS5'};
  check(state.rate===0&&state.bits===0&&!c.rlShaperSummary(metadata,state).includes('NS5'),lang+' stopped output never claims a measured automatic result');
  c.rlLastStatus={playing:true,outRate:192000,outBits:24,quantizationBits:24,outMode:'pcm'};state=c.rlShaperConditions();
  const summary=c.rlShaperSummary(metadata,state);check(summary.includes('NS5')&&summary.includes('192000')&&summary.includes('24'),lang+' format suggestion is tied to actual PCM output conditions');
  const suggestionKey='按格式建议：{0} · {1} Hz / {2} bit；实际应用见信号链。';
  check(typeof dict[suggestionKey]==='string'&&summary===dict[suggestionKey].replace('{0}','NS5').replace('{1}','192000').replace('{2}','24'),lang+' guidance labels metadata as a suggestion and directs actual processing to the signal chain');
  c.rlLastStatus.quantizationBits=18;state=c.rlShaperConditions();check(state.bits===18,lang+' auto guidance uses actual DAC quantization depth instead of its transport container');
  c.rlLastStatus.quantizationBits=0;state=c.rlShaperConditions();check(state.bits===0&&!c.rlShaperSummary(metadata,state).includes('NS5'),lang+' unavailable quantization depth stays unknown instead of assuming container depth');
  c.rlLastStatus.outBits=1;c.rlLastStatus.outMode='native';state=c.rlShaperConditions();check(state.dsd&&state.rate===0&&!c.rlShaperSummary(metadata,state).includes('NS5'),lang+' native DSD never claims PCM dither is active');
  c.rlLastStatus={playing:true,outRate:44100,outBits:16,quantizationBits:16,outMode:'pcm'};nodes.sShap.value='Shaped';state=c.rlShaperConditions();check(state.selected==='shaped',lang+' legacy Shaped spelling selects canonical metadata');
  const out={...metadata,options:options.map(o=>({...o,recommended:false}))};check(c.rlShaperSummary(out,state).includes(dict['当前输出条件不在建议范围内；可选择 auto / TPDF。']),lang+' unsuitable output conditions are explained without changing user selection');
 }
 let calls=0,resolve;const c={Promise,Array,Error,RL_SHAPER_INFO:{cache:{},pending:{}},apiGetRaw:()=>{calls++;return new Promise(r=>{resolve=r;});}};vm.createContext(c);vm.runInContext(fn('rlLoadShaperInfo'),c);
 const a=c.rlLoadShaperInfo(192000,24),b=c.rlLoadShaperInfo(192000,24);check(a===b&&calls===1,'concurrent guidance panels share one metadata request');resolve({ok:true,options});await a;await c.rlLoadShaperInfo(192000,24);check(calls===1,'stable output conditions reuse metadata instead of polling');
 c.apiGetRaw=async()=>{calls++;throw Error('offline');};try{await c.rlLoadShaperInfo(44100,16);}catch(e){}check(!c.RL_SHAPER_INFO.pending['44100/16'],'metadata failure clears pending state so user retry works');
 console.log(JSON.stringify({checks,failed,actualAudioFrames:0}));process.exitCode=failed?1:0;
})().catch(e=>{console.error(e);process.exitCode=2;});
