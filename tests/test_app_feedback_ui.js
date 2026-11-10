// Real UI functions with mocked local API responses; no feedback is sent.
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=process.env.RLPLAYER_TEST_SOURCE_ROOT||path.join(__dirname,'..');
const source=fs.readFileSync(path.join(root,'src/net/ui_script.h'),'utf8');
const start=source.indexOf('function fbSubmit('),fn=source.slice(start,source.indexOf('\n}',start)+2);
const showStart=source.indexOf('function fbShowResult('),show=source.slice(showStart,source.indexOf('\n}',showStart)+2);
let checks=0,failed=0;function check(ok,label){checks++;if(!ok)failed++;console.log((ok?'PASS ':'FAIL ')+label);}
function dictionary(lang){return JSON.parse(fs.readFileSync(path.join(root,'data/i18n/'+lang+'.json'),'utf8'));}
function context(lang,nodes,notices){const dict=dictionary(lang);return {dict,c:{D:id=>nodes[id]||null,toast:s=>notices.push(s),t:s=>dict[s]||s,
 tf:(s,args)=>(dict[s]||s).replace(/\{(\d+)\}/g,(_,i)=>args[i]),AbortController,clearTimeout:()=>{},setTimeout:()=>1}};}
function showResult(lang,result){const nodes={sp_fbmsg:{},sp_fbresult:{}},notices=[],{c}=context(lang,nodes,notices);
 vm.createContext(c);vm.runInContext(fn,c);vm.runInContext(show,c);c.fbShowResult(result);return {nodes,notices,dict:dictionary(lang)};}
async function runAsync(lang,result,{rejectStart=false,rejectStatus=false}={}){
 const nodes={sp_fbmsg:{},sp_fbresult:{},sp_fbsubmit:{},sp_fbtype:{value:'bug'},sp_fbtitle:{value:'Synthetic offline report'},sp_fbbody:{value:''},sp_fbsteps:{value:''}};
 const notices=[],{c,dict}=context(lang,nodes,notices),urls=[];let statusCalls=0;
 const response=url=>{urls.push(url);if(url==='/api/feedback'){
  if(rejectStart)return Promise.reject(Error('offline start fixture'));
  return Promise.resolve({ok:true,accepted:true,job:'job-offline-fixture'});
 }
 if(rejectStatus)return Promise.reject(Error('offline status fixture'));
 statusCalls++;return Promise.resolve(statusCalls===1?{ok:true,done:false}:{ok:true,done:true,result});};
 c.fetch=url=>Promise.resolve({json:async()=>response(url)});
 c.api=url=>response(url);
 c.setTimeout=(callback,delay)=>delay<=5000?setImmediate(callback):1;
 c.clearTimeout=handle=>{if(handle&&typeof handle==='object')clearImmediate(handle);};
 vm.createContext(c);vm.runInContext(show,c);vm.runInContext(fn,c);c.fbSubmit();
 for(let i=0;i<30;i++)await new Promise(r=>setImmediate(r));
 return {nodes,notices,dict,urls,statusCalls};
}
(async()=>{for(const lang of ['en','ja','fr','ru','es','ar']){
 const local=showResult(lang,{uploaded:false,status:503,local:'D:/private-fixture/report.json',error:'unavailable'});
 check(local.nodes.sp_fbmsg.textContent===local.dict['反馈已保存到本地'],lang+' local fallback status does not falsely claim missing credentials');
 check(local.nodes.sp_fbresult.textContent.includes(local.dict['⚠️ 未能自动上传，反馈已保存到：']),lang+' local fallback heading states outcome without inventing cause');
 check(local.nodes.sp_fbresult.textContent.includes(local.dict['把这个文件发到 Q 群 895816505 即可，内容与自动上传的完全一样。'])&&local.nodes.sp_fbresult.textContent.includes(local.dict['—— 技术细节（反馈给开发者时贴这段即可）——']),lang+' composed multiline result translates its guidance and technical heading');
 const invalid=showResult(lang,{uploaded:false,status:200,error:'反馈服务响应无效，无法确认已提交'});
 check(invalid.notices[0]===invalid.dict['反馈服务响应无效，无法确认已提交'],lang+' invalid acknowledgement error is localized');
 const success=showResult(lang,{uploaded:true,status:201,id:'FB-async',path:'FB-async',diag_stored:true});
 check(success.nodes.sp_fbresult.textContent.includes('status:   201')&&success.nodes.sp_fbmsg.textContent===success.dict['已上传，感谢反馈！'],lang+' final 201 job result keeps the success presentation');
 }
 const local=await runAsync('en',{uploaded:false,status:503,local:'D:/private-fixture/report.json',error:'unavailable'});
 check(local.urls[0]==='/api/feedback'&&local.statusCalls===2,'accepted/job UI polls pending and then completed status exactly once each');
 check(local.urls.slice(1).every(url=>url.includes('feedback')&&url.includes('job-offline-fixture')),'every status request addresses the accepted job');
 check(local.nodes.sp_fbresult.textContent.includes('D:/private-fixture/report.json')&&local.nodes.sp_fbsubmit.disabled===false,'UI renders done.result rather than treating the 202 acceptance as a final failure');
 const success=await runAsync('en',{uploaded:true,status:201,id:'FB-async',path:'FB-async',diag_stored:true});
 check(success.statusCalls===2&&success.nodes.sp_fbmsg.textContent===success.dict['已上传，感谢反馈！'],'UI renders a real completed 201 result after asynchronous acceptance');
 const lostStart=await runAsync('en',{}, {rejectStart:true});check(lostStart.nodes.sp_fbsubmit.disabled===false,'rejected initial request restores submit for retry');
 const lostStatus=await runAsync('en',{}, {rejectStatus:true});check(lostStatus.urls.length>2&&lostStatus.urls.filter(url=>url==='/api/feedback').length===1&&lostStatus.nodes.sp_fbsubmit.disabled===true,'status-read failure retries only status polling and never reposts the upload');
 console.log(JSON.stringify({checks,failed}));process.exit(failed?1:0);})();
