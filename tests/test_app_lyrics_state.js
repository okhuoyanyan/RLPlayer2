// Run the production UI functions with controlled, delayed HTTP responses.
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(process.env.RLPLAYER_TEST_SOURCE_ROOT || path.join(__dirname,'..'), 'src/net/ui_script.h'), 'utf8');
function extract(name) {
  const start = source.indexOf('function ' + name + '(');
  const end = source.indexOf('\n}', start + 1) + 2;
  return source.slice(start, end);
}
let checks = 0, failed = 0;
function check(ok, label) { ++checks; if (!ok) ++failed; console.log((ok ? 'PASS ' : 'FAIL ') + label); }
function setup() {
  const pending = [], notices = [];
  const c = { LY: { lines: [{s:'Intro'}, {s:'First'}], raw: 'equal' },
    LYVIEW: { lang:'en', data:{}, trans:1 }, I18N:{lang:'en'},
    AbortController, setTimeout:()=>1, clearTimeout:()=>{},
    lyricViewSave:()=>{}, applyLyricView:()=>{}, toast:s=>notices.push(s),
    fetch:()=>new Promise((resolve,reject)=>pending.push({resolve,reject})) };
  vm.createContext(c);
  for (const n of ['lyricKey','lyricTargetLang','lyricExtra','lyricAi']) vm.runInContext(extract(n),c);
  return { c,pending,notices };
}
const settle = () => new Promise(resolve=>setImmediate(resolve));
const reply = (p, lines) => p.resolve({ok:true,status:200,text:async()=>JSON.stringify({ok:true,lines})});
(async()=>{
  { const {c}=setup();const a=c.lyricKey();c.LY.lines[1].s='Other';check(a!==c.lyricKey(),'same-length lyrics with identical first line have distinct content identities'); }
  { const {c}=setup();c.LYVIEW.data.trans={key:c.lyricKey(),lang:'en',lines:['English']};c.LYVIEW.lang='es';check(c.lyricExtra('trans')===null,'English translation is unavailable after selecting Spanish'); }
  { const {c,pending,notices}=setup();c.lyricAi('trans');c.LY.lines=[{s:'New song'}];reply(pending[0],['Old translation']);await settle();check(!c.lyricExtra('trans'),'late response cannot be assigned to the newly selected song');check(notices.length===1,'stale result does not toast completion over new song'); }
  { const {c,pending}=setup();c.lyricAi('trans');c.LYVIEW.lang='es';reply(pending[0],['English']);await settle();check(!c.lyricExtra('trans'),'late English response cannot populate Spanish view'); }
  { const {c,pending}=setup();c.lyricAi('trans');c.lyricAi('trans');reply(pending[1],['Newest']);await settle();reply(pending[0],['Older']);await settle();check(c.lyricExtra('trans')?.[0]==='Newest','older same-task request cannot replace newer response'); }
  { const {c,pending}=setup();c.lyricAi('trans');c.lyricAi('trans');reply(pending[1],['Newest']);await settle();pending[0].reject(Error('old failure'));await settle();check(c.LYVIEW.trans===1,'stale failure cannot disable a newer successful translation'); }
  { const {c,pending}=setup();c.lyricAi('trans');reply(pending[0],['Valid']);await settle();check(c.lyricExtra('trans')?.[0]==='Valid','matching response is usable'); }
  { const {c,pending}=setup();c.lyricAi('trans');pending[0].reject(Error('network unavailable'));await settle();check(c.LYVIEW.trans===0,'current failure still resets toggle for retry'); }
  { const {c}=setup();const box={dataset:{},classList:{toggle:()=>{}},innerHTML:'',scrollTop:0};
    Object.assign(c,{D:id=>id==='npLyric'?box:null,lyricTimed:()=>false,lyricRows:()=>[],
      lyricSplitPair:s=>({main:s,sub:''}),esc:s=>s});
    vm.runInContext(extract('renderNpLyric'),c);c.renderNpLyric();c.LY.lines[1].s='Other';c.renderNpLyric();
    check(box.innerHTML.includes('Other')&&!box.innerHTML.includes('First'),'same-length new lyrics invalidate the rendered DOM');
    c.LYVIEW.data.trans={key:c.lyricKey(),lang:'en',lines:['English']};box.dataset.n='';c.renderNpLyric();
    c.LYVIEW.lang='es';c.renderNpLyric();check(!box.innerHTML.includes('English'),'language switch removes previously rendered English translation'); }
  console.log(JSON.stringify({checks,failed}));process.exitCode=failed?1:0;
})();
