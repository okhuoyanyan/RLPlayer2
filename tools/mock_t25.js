(function(){
  window.__msgs = [];
  window.__MOCKDATA = {
    track: { ok:true, kind:'track', summaryCached:false, unverified:true, citations:[], fields:{
      '演唱':'MOCK 演唱', '作词':'MOCK 作词', '时长':'02:42（Track 01）',
      '关联':'官方试听：https://monster-siren.hypergryph.com/music/779484' } },
    release: { ok:true, kind:'release', summaryCached:true, unverified:true, citations:[], fields:{
      '艺术家':'MOCK 艺术家', '时长':'02:42（Track 01）', '曲目列表':'01 MOCK 单曲 02:42',
      '专辑介绍':'MOCK 专辑介绍', '关联':'https://example.com/album/1' } },
    person: { ok:true, kind:'person', summaryCached:true, unverified:true, citations:[{id:'c1',title:'维基百科',url:'https://zh.wikipedia.org/wiki/MOCK'}], fields:{
      '简介':'MOCK 简介', '国籍':'MOCK 国籍' } }
  };
  if (window.__t25mock) return 1;
  window.__t25mock = 1;
  var of = window.fetch;
  window.fetch = function(u, o){
    var s = String(u);
    if (s.indexOf('/api/entity/summary') === 0) {
      var m = /[?&]kind=([a-z]+)/.exec(s);
      var kind = m ? m[1] : 'person';
      return Promise.resolve(new Response(JSON.stringify(window.__MOCKDATA[kind] || {ok:false,error:'mock-none'}), { status:200, headers:{'Content-Type':'application/json'} }));
    }
    if (s.indexOf('/api/cast/devices') === 0) {
      return Promise.resolve(new Response(JSON.stringify({ ok:true, n:1, devices:[{ name:'Mock-N1000-Renderer', friendlyName:'Mock-N1000-Renderer', model:'RLPlayer Mock Renderer', host:'127.0.0.1', port:'__MOCK_PORT__', location:'__MOCK_DEV__', avTransportUrl:'__MOCK_BASE__/ctl/avt', hasAVTransport:true }] }), { status:200, headers:{'Content-Type':'application/json'} }));
    }
    if (s.indexOf('/api/status') === 0) {
      return of.apply(this, arguments).then(function(r){ return r.json(); }).then(function(j){
        j.hasLlmKey = true; j.summaryEnabled = true;
        // 测试用：冻结播放进度，避免 400ms 的 /api/status 轮询把 S.pos 覆盖回真实值（测歌词滚动时需要）
        if (typeof window.__freezePos === 'number') { j.pos = window.__freezePos; if (window.__freezeDur) j.dur = window.__freezeDur; }
        return new Response(JSON.stringify(j), { status:200, headers:{'Content-Type':'application/json'} });
      });
    }
    return of.apply(this, arguments);
  };
  try { META.hasLlmKey = true; META.summary = 1; } catch(e){}
  return 1;
})()