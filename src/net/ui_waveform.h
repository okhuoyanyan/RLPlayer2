// Shared real-file waveform for the track page and A/B passage selection.
#pragma once
namespace rta::net {
inline const char* rta_ui_waveform_css() {
return R"WAVECSS(
.rlWaveform{width:100%;min-width:0;color:var(--txt2);font-variant-numeric:tabular-nums;direction:ltr}
.rlWavePlot{position:relative;width:100%;height:64px;touch-action:none;border-radius:7px;overflow:visible;background:transparent;isolation:isolate}
.rlWavePlot::before{content:'';position:absolute;inset:50% 0 auto;height:1px;background:var(--line);pointer-events:none}
.rlWavePlot>svg,.rlWavePlayed>svg{position:absolute;inset:0;width:100%;height:100%;pointer-events:none;overflow:visible;z-index:2}
.rlWaveBase{stroke:var(--mut2);opacity:.48}.rlWavePlayed .rlWaveBase{stroke:var(--acc);opacity:1}
.rlWavePlayed{position:absolute;inset:0;pointer-events:none;clip-path:inset(0 100% 0 0)}
.rlWaveHead{position:absolute;inset-block:4px;width:1px;background:var(--txt);pointer-events:none;z-index:2}
.rlWaveSeek{position:absolute;inset:0;width:100%;height:100%;margin:0!important;padding:0!important;opacity:0;z-index:3;cursor:pointer;touch-action:none}
.rlWaveform[data-drag-select=true] .rlWaveSeek{pointer-events:none}
.rlWaveform[data-drag-select=true] .rlWavePlot{cursor:crosshair}
.rlWaveSelection{cursor:grab}.rlWaveSelection:active{cursor:grabbing}
.rlWavePlot:has(.rlWaveSeek:focus-visible){outline:2px solid var(--acc);outline-offset:4px}
.rlWaveSelection{position:absolute;inset-block:0;background:rgba(217,184,117,.16);background:color-mix(in srgb,var(--acc) 16%,transparent);border-block:1px solid var(--acc);pointer-events:none;z-index:1}
/* 3.3.9：选区上显式显示「可整段拖动」的手柄提示，避免用户以为只能拉两端。 */
.rlWaveSelection::after{content:'⇄';position:absolute;left:50%;top:50%;transform:translate(-50%,-50%);font-size:12px;line-height:1;color:var(--acc);opacity:.55;pointer-events:none}
.rlWaveHandle{position:absolute;top:-1px;bottom:-1px;width:14px;min-width:14px;padding:0!important;transform:translateX(-50%);border:0;border-inline:2px solid var(--acc)!important;border-radius:3px;background:transparent!important;color:var(--acc);z-index:5;cursor:ew-resize;touch-action:none}
.rlWaveHandle::after{content:'';position:absolute;left:3px;top:40%;height:20%;width:2px;background:currentColor;box-shadow:3px 0 currentColor}
.rlWaveHandle:focus-visible{outline:2px solid var(--txt);outline-offset:3px}
.rlWaveMeta,.rlWaveRange{display:flex;align-items:center;gap:10px;justify-content:space-between;font-size:11px;color:var(--mut2);min-width:0;margin-top:5px}
.rlWaveState{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}.rlWaveTime{white-space:nowrap;flex:none;color:var(--txt2)}
.rlWaveRange{justify-content:flex-start;flex-wrap:wrap;gap:8px;margin-top:12px}.rlWaveRange label{display:flex;align-items:center;gap:5px}
.rlWaveRange input{width:78px;min-width:0;font-size:12px;padding:5px 6px;direction:ltr;font-variant-numeric:tabular-nums}
.rlWaveRange button,.rlWaveRetry{font:inherit;border:1px solid var(--line);border-radius:5px;padding:5px 8px;background:transparent;color:var(--txt2);cursor:pointer}
.rlWaveRetry{margin-inline-start:8px;padding:2px 6px}.rlWaveform [hidden]{display:none!important}
.rlWaveform[data-state=pending] .rlWaveState{color:var(--mut)}.rlWaveform[data-state=error] .rlWaveState{color:var(--warn)}
#npWaveform .rlWavePlot{height:58px}#npWaveform .rlWaveMeta{font-size:10.5px}
@media(max-height:700px){#npWaveform .rlWavePlot{height:42px}}
)WAVECSS";
}
inline const char* rta_ui_waveform_js() {
return R"WAVEJS(
(function(global){
  'use strict';
  function tr(value){return typeof t==='function'?t(value):value;}
  function safe(value){return String(value).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
  function number(value,fallback){value=Number(value);return Number.isFinite(value)?value:fallback;}
  function clamp(value,low,high){return Math.max(low,Math.min(high,value));}
  function time(value){value=Math.max(0,number(value,0));var whole=Math.floor(value);return Math.floor(whole/60)+':'+String(whole%60).padStart(2,'0')+'.'+Math.floor((value-whole)*10);}
  function mount(host,initial){
    if(!host)return null;
    if(host.rlWaveform)host.rlWaveform.destroy();
    var opts={},file='',identity='',dataKey='',generation=0,dead=false,timer=null,abort=null,bins=[],state='empty',message='',attempt=0;
    var gesture=false,cancelledGesture=false,gestureIdentity='',drag=null,position=0,duration=0,start=0,end=0;
    host.classList.add('rlWaveform');
    host.innerHTML='<div class=rlWavePlot><svg viewBox="0 0 1000 68" preserveAspectRatio=none aria-hidden=true><path class=rlWaveBase fill=none stroke-width=2></path></svg><div class=rlWavePlayed><svg viewBox="0 0 1000 68" preserveAspectRatio=none aria-hidden=true><path class=rlWaveBase fill=none stroke-width=2></path></svg></div><div class=rlWaveSelection hidden></div><i class=rlWaveHead></i><input class=rlWaveSeek type=range min=0 max=1 step=0.1 value=0 dir=ltr><button type=button class="rlWaveHandle rlWaveStart" role=slider hidden></button><button type=button class="rlWaveHandle rlWaveEnd" role=slider hidden></button></div><div class=rlWaveMeta><span class=rlWaveState role=status></span><button type=button class=rlWaveRetry hidden>'+safe(tr('重试'))+'</button><span class=rlWaveTime></span></div><div class=rlWaveRange hidden><label>'+safe(tr('起点'))+' <input class=rlWaveStartTime type=number min=0 step=0.1></label><button type=button class=rlWaveSetStart>'+safe(tr('将当前位置设为起点'))+'</button><label>'+safe(tr('终点'))+' <input class=rlWaveEndTime type=number min=0 step=0.1></label><button type=button class=rlWaveSetEnd>'+safe(tr('将当前位置设为终点'))+'</button></div>';
    function q(selector){return host.querySelector(selector);}
    var plot=q('.rlWavePlot'),seek=q('.rlWaveSeek'),played=q('.rlWavePlayed'),head=q('.rlWaveHead'),range=q('.rlWaveSelection'),startHandle=q('.rlWaveStart'),endHandle=q('.rlWaveEnd');
    seek.setAttribute('aria-label',tr('播放进度'));seek.title=tr('拖动或点击跳转。方向键移动 5 秒，Home / End 跳至两端。');
    startHandle.setAttribute('aria-label',tr('片段起点'));endHandle.setAttribute('aria-label',tr('片段终点'));
    startHandle.setAttribute('aria-keyshortcuts','ArrowLeft ArrowRight Home End');endHandle.setAttribute('aria-keyshortcuts','ArrowLeft ArrowRight Home End');
    function validDuration(){return duration>0&&!!file;}
    function paint(){
      if(dead)return;
      host.dataset.state=state;host.dataset.file=file;host.dataset.trackKey=identity;host.dataset.dragSelect=String(opts.dragSelect===true);
      var available=validDuration(),ratio=duration?clamp(position/duration,0,1):0;
      seek.disabled=!available||opts.disabled===true;seek.max=duration||1;
      if(!gesture)seek.value=clamp(position,0,duration||0);
      seek.setAttribute('aria-valuetext',time(number(seek.value,0))+' / '+time(duration));
      played.style.clipPath='inset(0 '+(100-ratio*100).toFixed(4)+'% 0 0)';head.style.left=(ratio*100)+'%';
      q('.rlWaveTime').textContent=time(gesture?number(seek.value,position):position)+' / '+time(duration);
      q('.rlWaveState').textContent=state==='ready'?tr('整曲波形'):state==='pending'?tr('正在生成整曲波形…'):state==='error'?tr('波形暂不可用，仍可拖动进度条'):tr('选择一首曲目后显示真实波形');
      q('.rlWaveState').title=state==='error'?message:'';q('.rlWaveRetry').hidden=state!=='error';
      var selection=opts.selection===true;
      range.hidden=startHandle.hidden=endHandle.hidden=!selection;q('.rlWaveRange').hidden=!selection||opts.rangeControls===false;
      if(selection){
        range.style.left=(duration?100*start/duration:0)+'%';range.style.width=(duration?100*(end-start)/duration:0)+'%';
        [[startHandle,start],[endHandle,end]].forEach(function(pair){pair[0].style.left=(duration?100*pair[1]/duration:0)+'%';pair[0].disabled=!available;pair[0].setAttribute('aria-valuemin','0');pair[0].setAttribute('aria-valuemax',String(duration));pair[0].setAttribute('aria-valuenow',String(pair[1]));pair[0].setAttribute('aria-valuetext',time(pair[1]));});
        ['.rlWaveStartTime','.rlWaveEndTime','.rlWaveSetStart','.rlWaveSetEnd'].forEach(function(s){q(s).disabled=!available;});
        [[q('.rlWaveStartTime'),start],[q('.rlWaveEndTime'),end]].forEach(function(pair){pair[0].max=duration; if(document.activeElement!==pair[0])pair[0].value=pair[1].toFixed(1);});
      }
    }
    function draw(){
      var path='';
      if(bins.length){var count=Math.min(1000,bins.length);for(var i=0;i<count;i++){var first=Math.floor(i*bins.length/count),last=Math.max(first+1,Math.floor((i+1)*bins.length/count)),peak=0;for(var k=first;k<last;k++)peak=Math.max(peak,bins[k]);var x=(i+.5)*1000/count,h=Math.max(0.6,peak*31);path+='M'+x.toFixed(2)+' '+(34-h).toFixed(2)+'V'+(34+h).toFixed(2);}}
      host.querySelectorAll('path.rlWaveBase').forEach(function(p){p.setAttribute('d',path);});
      paint();
    }
    function cancel(){clearTimeout(timer);timer=null;if(abort){abort.abort();abort=null;}}
    function load(myGeneration,fresh){
      if(dead||myGeneration!==generation||!file)return;
      var requestFile=file;state='pending';paint();abort=new AbortController();
      fetch('/api/waveform?file='+encodeURIComponent(requestFile)+'&clip_start='+encodeURIComponent(Math.max(0,number(opts.clipStart,0)))+'&clip_length='+encodeURIComponent(Math.max(0,number(opts.clipLength,0)))+(fresh?'&fresh=1':''),{signal:abort.signal,cache:'no-store'}).then(function(response){if(!response.ok)throw Error('HTTP '+response.status);return response.json();}).then(function(data){
        if(dead||myGeneration!==generation||requestFile!==file)return;
        abort=null;
        if(data&&data.ok!==false&&data.state==='pending'){
          if(++attempt<480){timer=setTimeout(function(){load(myGeneration);},500);return;}
          throw Error(tr('生成时间较长，请重试'));
        }
        if(!data||data.ok===false||data.state!=='ready'||!Array.isArray(data.bins)||!data.bins.length)throw Error(data&&data.error||tr('没有可用的波形数据'));
        if(data.bins.some(function(v){return typeof v!=='number'||!Number.isFinite(v)||v<0||v>1;}))throw Error(tr('波形数据无效'));
        var value={duration:number(data.duration,0),bins:data.bins.slice()};
        bins=value.bins;if(!duration&&value.duration>0)duration=value.duration;state='ready';message='';draw();
      }).catch(function(error){if(dead||myGeneration!==generation||requestFile!==file||error.name==='AbortError')return;abort=null;bins=[];state='error';message=String(error.message||error);draw();});
    }
    function setRange(a,b,committed){
      if(!validDuration())return;
      var gap=Math.min(.01,duration);start=clamp(number(a,start),0,Math.max(0,duration-gap));end=clamp(number(b,end),start+gap,duration);
      paint();if(typeof opts.onRange==='function')opts.onRange(start,end,committed===true);
    }
    function update(patch){
      if(dead)return;
      var oldIdentity=identity;Object.assign(opts,patch||{});file=String(opts.file||'');dataKey=JSON.stringify([file,Math.max(0,number(opts.clipStart,0)),Math.max(0,number(opts.clipLength,0))]);identity=JSON.stringify([dataKey,String(opts.trackKey||'')]);duration=Math.max(0,number(opts.duration,0));if(!gesture||identity!==oldIdentity)position=clamp(number(opts.position,position),0,duration||0);
      if(identity!==oldIdentity){
        ++generation;cancel();bins=[];message='';attempt=0;cancelledGesture=gesture||!!drag;gesture=false;gestureIdentity='';drag=null;selectDrag=null;
        start=0;end=duration;state=file?'pending':'empty';
        // Ask the source-aware backend cache on every identity change. A path-only
        // browser cache would reuse obsolete bins after the file changed in place.
        if(file)load(generation);
      }
      if(patch&&Object.prototype.hasOwnProperty.call(patch,'start'))start=number(patch.start,0);
      if(patch&&Object.prototype.hasOwnProperty.call(patch,'end'))end=number(patch.end,duration);
      start=clamp(start,0,Math.max(0,duration-.01));end=clamp(end,start+Math.min(.01,duration),duration);
      draw();
    }
    function preview(){if(gestureIdentity!==identity)return;position=number(seek.value,0);paint();}
    // 3.3.8：波形上直接拖拽框选片段；轻点仍是跳转。手柄仍可单独微调。
    // 3.3.9：① 在已选中的片段内部按下 = 整段平移，不再重新框选；
    //        ② 拖手柄/整段平移/框选过程中按帧触发 onScrub，交给播放器做跟手试听。
    var selectDrag=null,scrubTimer=0,lastScrub=-1;
    function scrubbing(value){
      if(typeof opts.onScrub!=='function')return;
      var now=(typeof performance!=='undefined'&&performance.now)?performance.now():Date.now();
      if(now-scrubTimer<110)return;
      scrubTimer=now;
      if(Math.abs(value-lastScrub)<0.02)return;
      lastScrub=value;opts.onScrub(value);
    }
    function insideSelection(event,at){return opts.dragSelect===true&&at>start+duration*0.004&&at<end-duration*0.004;}
    plot.addEventListener('pointerdown',function(event){
      if(opts.dragSelect!==true||!validDuration()||event.button!==0||event.target.closest('.rlWaveHandle'))return;
      event.preventDefault();
      var at=point(event);
      if(insideSelection(event,at)){
        // 整段拖动：记录按下点相对片段起点的偏移，之后整体平移。
        selectDrag={id:event.pointerId,x:event.clientX,mode:'move',at:at,start:start,end:end,moved:false};
      }else{
        selectDrag={id:event.pointerId,x:event.clientX,mode:'select',at:at,moved:false};
      }
      try{plot.setPointerCapture(event.pointerId);}catch(e){}
    });
    plot.addEventListener('pointermove',function(event){
      if(!selectDrag||selectDrag.id!==event.pointerId)return;
      if(Math.abs(event.clientX-selectDrag.x)>2)selectDrag.moved=true;
      if(!selectDrag.moved)return;
      var at=point(event);
      if(selectDrag.mode==='move'){
        var span=selectDrag.end-selectDrag.start,delta=at-selectDrag.at;
        var nextStart=clamp(selectDrag.start+delta,0,Math.max(0,duration-span));
        setRange(nextStart,nextStart+span,false);scrubbing(nextStart);
      }else{
        setRange(Math.min(selectDrag.at,at),Math.max(selectDrag.at,at),false);scrubbing(at);
      }
    });
    var endSelect=function(event){
      if(!selectDrag||selectDrag.id!==event.pointerId)return;
      var at=point(event),drag0=selectDrag;selectDrag=null;
      if(drag0.mode==='move'){
        if(drag0.moved){var span0=drag0.end-drag0.start,delta0=at-drag0.at;var s0=clamp(drag0.start+delta0,0,Math.max(0,duration-span0));setRange(s0,s0+span0,true);}
        else if(typeof opts.onSeek==='function')opts.onSeek(at);
      }else if(drag0.moved){
        setRange(Math.min(drag0.at,at),Math.max(drag0.at,at),true);
      }else if(typeof opts.onSeek==='function')opts.onSeek(at);
    };
    plot.addEventListener('pointerup',endSelect);plot.addEventListener('pointercancel',function(){selectDrag=null;});
    function commitSeek(){var allowed=!cancelledGesture&&gestureIdentity===identity&&!seek.disabled;gesture=false;cancelledGesture=false;if(allowed){position=number(seek.value,0);if(typeof opts.onSeek==='function')opts.onSeek(position);}paint();}
    seek.addEventListener('pointerdown',function(){gesture=true;cancelledGesture=false;gestureIdentity=identity;});
    seek.addEventListener('input',function(){if(cancelledGesture)return;if(!gesture){gesture=true;gestureIdentity=identity;}preview();});
    seek.addEventListener('change',commitSeek);
    seek.addEventListener('pointercancel',function(){gesture=false;cancelledGesture=true;gestureIdentity='';paint();});
    seek.addEventListener('keydown',function(event){
      var delta={ArrowLeft:-5,ArrowDown:-5,ArrowRight:5,ArrowUp:5};if(!(event.key in delta)&&event.key!=='Home'&&event.key!=='End')return;
      event.preventDefault();event.stopPropagation();if(seek.disabled)return;
      cancelledGesture=false;gestureIdentity=identity;seek.value=clamp(event.key==='Home'?0:event.key==='End'?duration:number(seek.value,0)+delta[event.key],0,duration);commitSeek();
    });
    function point(event){var box=plot.getBoundingClientRect();return clamp((event.clientX-box.left)/Math.max(1,box.width),0,1)*duration;}
    function moveHandle(which,value,committed){if(which==='start')setRange(Math.min(value,end-.01),end,committed);else setRange(start,Math.max(value,start+.01),committed);}
    [[startHandle,'start'],[endHandle,'end']].forEach(function(pair){var button=pair[0],which=pair[1];
      button.addEventListener('pointerdown',function(event){if(button.disabled)return;event.preventDefault();event.stopPropagation();drag={which:which,file:identity,id:event.pointerId};button.setPointerCapture(event.pointerId);moveHandle(which,point(event),false);});
      button.addEventListener('pointermove',function(event){if(!(drag&&drag.which===which&&drag.file===identity))return;var at=point(event);moveHandle(which,at,false);scrubbing(which==='start'?at:at);});
      button.addEventListener('pointerup',function(event){if(drag&&drag.which===which&&drag.file===identity){moveHandle(which,point(event),true);drag=null;}});
      button.addEventListener('pointercancel',function(){drag=null;});
      button.addEventListener('keydown',function(event){var delta={ArrowLeft:-.1,ArrowDown:-.1,ArrowRight:.1,ArrowUp:.1};if(!(event.key in delta)&&event.key!=='Home'&&event.key!=='End')return;event.preventDefault();event.stopPropagation();if(button.disabled)return;var at=which==='start'?start:end;moveHandle(which,event.key==='Home'?0:event.key==='End'?duration:at+delta[event.key]*(event.shiftKey?10:1),true);});
    });
    q('.rlWaveSetStart').onclick=function(){setRange(Math.min(position,end-.01),end,true);};q('.rlWaveSetEnd').onclick=function(){setRange(start,Math.max(position,start+.01),true);};
    q('.rlWaveStartTime').onchange=function(){setRange(Math.min(number(this.value,start),end-.01),end,true);};q('.rlWaveEndTime').onchange=function(){setRange(start,Math.max(number(this.value,end),start+.01),true);};
    q('.rlWaveRetry').onclick=function(){++generation;cancel();attempt=0;message='';load(generation,true);};
    var api={update:update,destroy:function(){if(dead)return;dead=true;++generation;cancel();delete host.rlWaveform;host.innerHTML='';},setSelection:function(a,b){setRange(a,b,false);}};
    host.rlWaveform=api;update(initial||{});return api;
  }
  global.rlMountWaveform=mount;global.RLWaveform={mount:mount};
})(window);
)WAVEJS";
}
} // namespace rta::net
