#pragma once
namespace rta::net {
inline const char* rta_ui_studio_css(){return R"STUDIO(
#rlStudio [hidden]{display:none!important}
#rlStudio[hidden],html[data-rl-mode=pro] #rlListenHome>.rlListenHeading,html[data-rl-mode=pro] #rlListenHome>.rlListenGrid,html[data-rl-mode=pro] #rlListenHome>#rlHomeShelf{display:none}
#rlStudio{--desk-wood:#29241d;--desk-panel:#342d24;--desk-field:#211e19;--desk-line:#66543b;--desk-brass:#d9b875;--desk-text:#f1e6cf;--desk-muted:#c8b89d;--rl-title:var(--desk-text);--rl-copy:#e1d5bf;--rl-muted:var(--desk-muted);--rl-line:var(--desk-line);--rl-panel:var(--desk-panel);--rl-field:var(--desk-field);--rl-accent:var(--desk-brass);--rl-ink:#2b2112;--rl-tint:#51422c;color:var(--desk-text);background:var(--desk-wood);border:1px solid var(--desk-line);border-radius:10px;padding:18px 20px;text-align:start;container-type:inline-size;box-shadow:0 14px 30px #0002,inset 0 1px #fff1}
/* 2026-11-12：专业模式不再固定深棕 —— 白/浅蓝主题各自给出同材质的浅色台面，
   否则外壳是白的、中间一块深棕，割裂感就是用户说的「配色很突兀」。 */
html[data-rl-mode=pro][data-theme=light] #rlStudio{--desk-wood:#f4eddf;--desk-panel:#fffaf0;--desk-field:#f1e7d4;--desk-line:#c6b69b;--desk-brass:#7a5426;--desk-text:#342c24;--desk-muted:#6b5948;--rl-ink:#fff9ef}
html[data-rl-mode=pro][data-theme=blue] #rlStudio{--desk-wood:#e6eef2;--desk-panel:#f7fbfd;--desk-field:#edf4f8;--desk-line:#aebfc7;--desk-brass:#2d6271;--desk-text:#193746;--desk-muted:#476371;--rl-ink:#ffffff}
html[data-rl-mode=pro] #rlListenHome{padding-top:16px}#rlStudio *{box-sizing:border-box}#rlStudio button,#rlStudio select,#rlStudio input{font:inherit}#rlStudio button{cursor:pointer}#rlStudio button:disabled{opacity:.45;cursor:default}#rlStudio .btn{min-height:32px;padding:6px 12px;border-radius:4px;border:1px solid var(--desk-line);background:var(--desk-panel);color:var(--desk-text);font-size:12px;white-space:normal}#rlStudio .btn.pri{background:var(--desk-brass);color:#2b2112;border-color:var(--desk-brass)}
.digitalHeader,.digitalSectionHeading,.digitalCurrent,.digitalToolbar,.digitalSegmentTools,.digitalMonitorHeader{display:flex;align-items:center;gap:10px;justify-content:space-between}.digitalHeader{padding-bottom:12px}.digitalHeader h1{font:600 27px/1.2 var(--rl-serif,Georgia,serif);margin:0}.digitalHeader p{font-size:11px;color:var(--desk-muted);margin:4px 0 0}.digitalState{font:11px ui-monospace,Consolas,monospace;color:var(--desk-muted);display:flex;align-items:center;gap:7px}.digitalState:before{content:'';height:6px;width:6px;border-radius:50%;background:currentColor}.digitalState[data-state=running]{color:var(--desk-brass)}.digitalCurrent{justify-content:flex-start;padding:12px 0;border-top:1px solid var(--desk-line)}.digitalCover{height:48px;width:48px;flex:none;border-radius:4px;background:var(--desk-panel) center/cover;border:1px solid var(--desk-line)}.digitalTrack{min-width:0;flex:1}.digitalTrack h2{font:600 18px/1.3 var(--rl-serif,Georgia,serif);margin:0;overflow-wrap:anywhere}.digitalTrack p{font-size:11px;color:var(--desk-muted);margin:4px 0 0;overflow-wrap:anywhere}.digitalMessage{min-height:18px;color:var(--desk-muted);font-size:12px;line-height:1.5;margin:9px 0}.digitalMessage[data-error=true]{color:var(--rl-error,#ffbdad)}.digitalTopMessage{margin:0;max-width:50%;text-align:end}.digitalTopMessage:empty{display:none}.digitalFootnote{color:var(--desk-muted);font-size:11px;line-height:1.6;margin:8px 0}.rlRawVolumeNotice{font-size:11px;color:var(--desk-muted)}
#rlStudio #rlProPath{display:block;padding:0;margin:0 0 16px;background:var(--desk-field);border:1px solid var(--desk-line);border-radius:5px}#rlStudio #rlProPath .rlQueueHeading{padding:7px 11px;border-bottom:1px solid var(--desk-line)}#rlStudio .rlQueueHeading h2{font-size:12px;margin:0}#rlStudio .rlPathNodes{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:0;padding:0}#rlStudio .rlPathNode{margin:0;border:0;border-inline-end:1px solid var(--desk-line);padding:9px 11px;background:none}#rlStudio .rlPathNode:last-child{border:0}#rlStudio .rlPathNode small{display:block;font-size:10px;color:var(--desk-muted);margin-bottom:4px}#rlStudio .rlPathNode strong{font:600 13px/1.5 ui-monospace,Consolas,monospace;overflow-wrap:anywhere}.digitalSignalDetail{font-size:10px;color:var(--desk-muted);display:block;margin-top:4px;overflow-wrap:anywhere}.digitalSignalDetail:empty{display:none}
.digitalTabs{display:flex;gap:24px;border-bottom:1px solid var(--desk-line);margin-bottom:16px}.digitalTabs button{background:none;border:0;border-bottom:3px solid transparent;color:var(--desk-muted);padding:10px 2px 11px;font:600 15px/1.2 var(--rl-serif,Georgia,serif)}.digitalTabs button[aria-selected=true]{color:var(--desk-brass);border-bottom-color:var(--desk-brass)}.digitalPanel[hidden]{display:none}.digitalSectionHeading{margin:0 0 8px}.digitalSectionHeading h2{font:600 16px/1.3 var(--rl-serif,Georgia,serif);margin:0}.digitalCompareReading{font-size:11px;color:var(--desk-muted)}.digitalSlots{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:9px;margin:0 0 16px}.digitalSlot{display:grid;grid-template-columns:28px minmax(0,1fr);gap:3px 8px;text-align:start;min-width:0;padding:11px 12px;border:1px solid var(--desk-line);border-radius:5px;background:var(--desk-field);color:var(--desk-text)}.digitalSlot[aria-current=true]{border-color:var(--desk-brass);box-shadow:inset 0 2px var(--desk-brass);background:var(--desk-field)}.digitalSlotCode{grid-row:span 3;color:var(--desk-brass);font:600 24px/1.1 Georgia,serif;align-self:center}.digitalSlotName{font-size:13px;min-width:0;overflow:hidden;white-space:nowrap;text-overflow:ellipsis}.digitalSlotName input{width:100%;min-width:0;padding:2px 3px;background:var(--desk-field);border:1px solid var(--desk-brass);color:var(--desk-text)}.digitalSlotSettings{font:10px/1.5 ui-monospace,Consolas,monospace;color:var(--desk-muted);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.digitalSlotState{font-size:10px;color:var(--desk-brass)}
.digitalRack{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));border:1px solid var(--desk-line);border-radius:5px;overflow:hidden}.digitalModule{padding:15px;min-width:0;background:var(--desk-panel);border-inline-end:1px solid var(--desk-line)}.digitalModule:last-child{border:0}.digitalModule>header{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:12px}.digitalModule h2{font:600 16px/1.3 var(--rl-serif,Georgia,serif);margin:0}.digitalFields{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}.digitalField{display:flex;flex-direction:column;gap:5px;min-width:0;font-size:11px;color:var(--desk-muted)}.digitalField.wide{grid-column:1/-1}.digitalField select,.digitalField input,.digitalQuickEq select,.digitalSegmentTools input{width:100%;min-width:0;padding:7px 8px;border:1px solid var(--desk-line);background:var(--desk-field);color:var(--desk-text);border-radius:3px;min-height:33px;font-size:12px}.digitalCheck{display:flex;align-items:center;gap:7px;color:var(--desk-text);font-size:12px}.digitalCheck.wide{grid-column:1/-1}.digitalCheck input{width:15px;height:15px;accent-color:var(--desk-brass)}.digitalReading{color:var(--desk-muted);font-size:10px;margin:11px 0 0;line-height:1.6;overflow-wrap:anywhere}.digitalTag{font-size:10px;color:var(--desk-brass)}.digitalQuickEq{display:flex;gap:10px;align-items:center;border:1px solid var(--desk-line);border-radius:5px;padding:10px 12px;margin-top:10px;font-size:12px}.digitalQuickEq label{flex:1;min-width:0}.digitalQuickEq select{margin-inline-start:10px;max-width:340px}.digitalSegment{margin-top:18px;padding-top:14px;border-top:1px solid var(--desk-line)}.digitalSegmentTools{flex-wrap:wrap;font-size:11px;color:var(--desk-muted)}.digitalSegmentTools label{display:flex;align-items:center;gap:7px}.digitalSegmentTools input[type=number]{width:93px;min-height:29px;font:11px ui-monospace,Consolas,monospace}.digitalSegmentTools output{font:12px ui-monospace,Consolas,monospace;color:var(--desk-brass)}.digitalWaveform{min-height:96px;margin:10px 0;background:var(--desk-field);border:1px solid var(--desk-line);border-radius:5px;overflow:hidden}
/* 2026-11-12：同曲片段重做 —— 不再是一个 96px 的空黑盒；波形铺在浅色/木色台面上，空态也有说明。 */
.rlSegQuick{display:flex;align-items:center;gap:10px;flex-wrap:wrap}.rlSegWaveWrap{position:relative;margin:10px 0 4px}
.rlSegWave{min-height:0;margin:0;background:transparent;border:0;border-radius:0;overflow:visible}
.rlSegWave .rlWavePlot{height:78px;background:var(--desk-field);border:1px solid var(--desk-line);border-radius:5px}
.rlSegWave .rlWaveMeta{margin-top:7px;font-size:11px}
.rlSegReadout{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-top:8px;font-size:12px;color:var(--desk-muted)}
.rlSegReadout output{font:600 14px/1.2 ui-monospace,Consolas,monospace;color:var(--desk-brass);letter-spacing:.02em}
.rlSegReadout .rlSegLead{display:inline-flex;align-items:center;gap:7px;font-size:11px}
.rlSegReadout .rlSegLead::before{content:'';width:16px;height:1px;background:var(--desk-brass);box-shadow:0 0 0 1px var(--desk-brass) inset}
.rlSegWave .rlWaveRange{display:none!important}
.rlSegHint{font-size:11px;line-height:1.6;color:var(--desk-muted);margin:8px 0 0;min-height:18px}
.rlSegHint[data-error=true]{color:var(--rl-error,#ffbdad)}
#rlStudioSegClear{min-height:28px;padding:4px 10px;font-size:11px}.digitalQueue{margin-top:18px;border-top:1px solid var(--desk-line);padding-top:12px}.digitalQueueRow{display:grid;grid-template-columns:28px minmax(0,1fr) auto;gap:8px;align-items:center;border:0;border-bottom:1px solid var(--desk-line);color:var(--desk-muted);background:none;padding:7px 1px;width:100%;text-align:start}.digitalQueueRow[aria-current=true]{color:var(--desk-brass)}.digitalQueueRow>span:nth-child(2){overflow:hidden;white-space:nowrap;text-overflow:ellipsis;font-size:12px}.digitalQueueRow small,.digitalQueueRow>span:first-child{font:10px ui-monospace,Consolas,monospace}
.digitalAdvanced{box-sizing:border-box;position:fixed;inset:0;margin:auto;max-width:800px;width:calc(100vw - 36px);max-height:calc(100dvh - 36px);overflow:auto;background:var(--desk-wood,#29241d);color:var(--desk-text,#f1e6cf);border:1px solid var(--desk-line,#66543b);border-radius:8px;padding:20px;box-shadow:0 22px 80px #0008}.digitalAdvanced::backdrop{background:#100e0bc9}.digitalAdvanced header{display:flex;align-items:center;justify-content:space-between;gap:15px;margin-bottom:15px}.digitalAdvanced h2{font:600 22px var(--rl-serif,Georgia,serif);margin:0}.digitalAdvanced .digitalFields{grid-template-columns:repeat(2,minmax(0,1fr))}.digitalAdvanced .digitalField small{font-size:10px;line-height:1.5}.digitalAdvanced input[type=checkbox]{width:16px;min-height:16px;align-self:flex-start;accent-color:var(--desk-brass)}.digitalAdvanced footer{display:flex;justify-content:space-between;align-items:center;border-top:1px solid var(--desk-line);padding-top:12px;margin-top:16px;font-size:11px;color:var(--desk-muted)}#rlStudio :focus-visible{outline:2px solid var(--desk-brass);outline-offset:3px}.digitalMonitorHeader{margin-bottom:12px;align-items:flex-start}.digitalMonitorHeader h2{font:600 20px/1.3 var(--rl-serif,Georgia,serif);margin:0}.digitalMonitorHeader p{margin:4px 0;font-size:12px;color:var(--desk-muted)}
@container(max-width:850px){.digitalRack{grid-template-columns:1fr 1fr}.digitalModule:nth-child(2){border:0}.digitalModule:last-child{grid-column:1/-1;border-top:1px solid var(--desk-line)}.digitalSlots{gap:6px}.digitalSlot{padding:10px 8px;grid-template-columns:23px minmax(0,1fr)}.digitalSlotCode{font-size:21px}.digitalQuickEq{flex-wrap:wrap}}
@container(max-width:530px){.digitalRack{grid-template-columns:1fr}.digitalModule{border:0;border-bottom:1px solid var(--desk-line)}.digitalModule:last-child{border:0}.digitalSlots{grid-template-columns:repeat(2,minmax(0,1fr))}.digitalTabs{gap:17px}.digitalTabs button{font-size:13px}.digitalTopMessage{max-width:100%;text-align:start}.digitalHeader{flex-wrap:wrap}.digitalQuickEq label{flex-basis:100%}.digitalQuickEq select{margin:7px 0 0;max-width:none}.digitalCurrent{flex-wrap:wrap}.digitalTrack{min-width:130px}#rlStudio .rlPathNodes{grid-template-columns:1fr}#rlStudio .rlPathNode{border:0;border-bottom:1px solid var(--desk-line)}.digitalAdvanced .digitalFields{grid-template-columns:1fr}}
@media(forced-colors:active){.digitalSlot[aria-current=true],.digitalTabs button[aria-selected=true]{outline:2px solid Highlight}}
)STUDIO";}
inline const char* rta_ui_studio_js(){return R"STUDIO(
var RL_STUDIO={pending:false,switching:false,instantSelect:false,selected:'A',frame:null,subscribed:false,tab:'ab',draft:{},inflight:{},editVersion:0,timer:0,waveform:null,range:{start:0,end:0},rangeFile:'',rangeIdentity:'',rangeVersion:0,rename:null};
function rlStudioEnabled(value){return value===true||String(value)==='1';}
function rlStudioCurrent(){return typeof rlConsole!=='undefined'?rlConsole.state:null;}
function rlStudioOption(value,label){return '<option value="'+esc(value)+'">'+esc(t(label))+'</option>';}
function rlStudioField(id,label,options,wide,key){return '<label class="digitalField'+(wide?' wide':'')+'"><span>'+esc(t(label))+'</span><select id="'+id+'"'+(key?' data-studio-key="'+key+'"':'')+'>'+options+'</select></label>';}
// 2026-11-12：DSD 调制模块也把 SDM Oversampling 1x/Nx 放到外面 —— 与 PCM 滤波的 1x/Nx 同构，
// 不再只藏在「高级」对话框里。留空 = 跟随 PCM 同名滤波。
function rlStudioSdmFilterOptions(sourceId,label){var src=D(sourceId);return '<option value="">'+esc(t(label))+'</option>'+(src?src.innerHTML:'');}
var RL_STUDIO_ADVANCED={
 pcm:[['crossfeed','启用耳机交叉馈送','check',null,'耳机交叉馈送会改变声像和频响；默认关闭。'],['crossfeed_freq','交叉馈送截止频率（Hz）','number',[300,1200,10],'低于此频段的信号会少量送入另一声道。'],['crossfeed_level','交叉声道衰减（dB）','number',[3,15,.5],'值越大效果越轻；12 dB 轻度，6 dB 标准。'],['phase','相位','select',[['','跟随滤波器预设'],['linear','线性相位'],['minimum','最小相位'],['intermediate','混合相位']],'强制相位会覆盖所选滤波器的相位。'],['taps','原型抽头数','number',[0,2097152,1024],'0 使用预设自动长度。'],['atten','阻带衰减 · dB','number',[0,300,1],'0 使用预设；实际衰减仍取决于抽头数。'],['transition','过渡带占比','number',[0,.45,.005],'0 使用预设；0.04 表示 4%。'],['isp','过采样峰值保护','check',null,'插值峰值超满幅时预衰减。'],['cleanup','启用录音噪声清理','check',null,'只在 PCM 域处理，会改变样本。'],['cleanup_auto','自动估计噪声底','check',null,'证据不足时不降噪。'],['cleanup_strength','清理强度','number',[0,1,.05],'0 至 1，较大值可能影响微弱细节。'],['cleanup_noise_db','手动噪声底 · dBFS','number',[-120,-60,.1],'仅手动模式使用。']],
 sdm:[['modcut','噪声整形拐点 · kHz','number',[0,200,5],'0 使用默认 100 kHz；非 EC 仅允许 0 或 100 至 200 kHz。'],['moddith','调制器抖动 · LSB','number',[-1,1,.001],'-1 使用默认值。'],['oobgain','带外增益','number',[0,20,.01],'0 跟随调制器默认值；实际稳定性以信号链为准。'],['ahmbits','AHM 位数','select',[[6,'6 bit'],[8,'8 bit']],'实时 DSD 仍是 1 bit；多电平由离线工具导出。'],['dsdkind','DSD 源抽取器','select',[['fir','FIR · Kaiser'],['iir','IIR · Butterworth'],['cic','CIC · 6 级']],'用于 DSD 源转 PCM；直通时跳过。'],['dsdconv','DSD 源转换通带','select',[['narrow','narrow · 20 kHz'],['wide','wide · 30 kHz'],['xfi','XFi · 40 kHz']],'通带越宽，需要的抽头越多。'],['dsd2stage','DSD 两段抽取','check',null,'使用两段方式完成 DSD 源抽取。'],['directsdm','同速率 DSD 尝试直通','check',null,'需源与目标同速率、音量 0 dB、无 EQ 或卷积等处理。']],
 output:[['bits','PCM 输出位深','select',[[0,'自动'],[16,'16 bit'],[24,'24 bit'],[32,'32 bit']],'DSD 输出恒为 1 bit。'],['channels','输出声道数','select',[[0,'跟随源'],[1,'1'],[2,'2'],[6,'5.1 · 6'],[8,'7.1 · 8']],'与硬件布局对应。'],['trim','各声道增益 · dB','text',null,'按声道顺序填写 8 个值，以英文逗号分隔，例如 0,0,0,0,0,0,0,0。'],['loudness','启用响度补偿','check',null,'低音量时补偿听感。'],['rgmode','ReplayGain','select',[['off','关闭'],['track','按单曲'],['album','按专辑']],'使用文件里的响度标签。'],['rgclip','ReplayGain 防削波','check',null,'按峰值标签限制增益。'],['eq','外部 EQ 文件','text',null,'保留已有外部校正文件；导入耳机 EQ 使用调音台。'],['ir','卷积文件','text',null,'本机 IR 文件完整路径。'],['irgpu','GPU 卷积','check',null,'设备支持时使用 GPU。'],['roomcorrauto','自动应用房间校正','check',null,'仅布局匹配时应用已有校正集。']]
};
function rlInstallStudio(){
 var home=D('rlListenHome'),path=D('rlProPath');if(!home||!path||D('rlStudio'))return;
 var studio=document.createElement('section');studio.id='rlStudio';studio.hidden=true;studio.setAttribute('aria-labelledby','rlStudioHeading');
 studio.innerHTML='<header class=digitalHeader><div><h1 id=rlStudioHeading>数字音乐台</h1><p>细调每一组声音，随时对比同一片段</p></div><p class="digitalMessage digitalTopMessage" id=rlStudioMessage role=status aria-live=polite></p><span class=digitalState id=rlStudioState></span></header>'
 +'<div class=digitalCurrent><div class=digitalCover id=rlStudioCover aria-hidden=true></div><div class=digitalTrack><h2 id=rlStudioTrack></h2><p id=rlStudioMeta></p></div></div><p class=digitalFootnote id=rlStudioRawVolumeNotice hidden></p><div id=rlStudioSignalHost></div>'
 +'<nav class=digitalTabs role=tablist aria-label="数字音乐台"><button id=rlStudioTab_ab type=button role=tab aria-selected=true aria-controls=rlStudioPanel_ab data-studio-tab=ab>播放台</button><button id=rlStudioTab_eq type=button role=tab aria-selected=false aria-controls=rlStudioPanel_eq data-studio-tab=eq tabindex=-1>EQ 调音台</button><button id=rlStudioTab_monitor type=button role=tab aria-selected=false aria-controls=rlStudioPanel_monitor data-studio-tab=monitor tabindex=-1>音质监控</button></nav>'
 +'<section id=rlStudioPanel_ab class=digitalPanel role=tabpanel aria-labelledby=rlStudioTab_ab><header class=digitalSectionHeading><h2>四组独立调音</h2><span class=digitalCompareReading id=rlStudioCompareState>正在读取当前组…</span></header><div class=digitalSlots id=rlStudioSlots></div>'
 +'<div class=digitalRack><section class=digitalModule data-studio-group=filter><header><h2>PCM 滤波</h2><span style="display:flex;gap:6px"><button class=btn type=button data-filter-browse=filter>滤波器详情</button><button class=btn type=button data-studio-advanced=pcm>高级</button></span></header><div class=digitalFields>'
 +rlStudioField('rlStudioFilter1','1x 音源滤波','',true,'filter')+rlStudioField('rlStudioFilterNx','Nx 音源滤波','',true,'filterNx')+rlStudioField('rlStudioTwoStage','两段升频','',false,'twostage')+rlStudioField('rlStudioShaper','PCM 抖动 / 噪声整形','',false,'shaper')+rlStudioField('rlStudioDacBits','DAC 有效位深',rlStudioOption(0,'自动（跟随输出容器）')+Array.from({length:25},function(_,i){return rlStudioOption(i+8,(i+8)+' bit');}).join(''),false,'dacbits')+'</div><p class=digitalReading id=rlStudioReading_filter></p><p class=digitalFootnote>'+esc(t('仅在已知 DAC 精度时手动设置；18/20 bit 可装入 24/32 bit 容器，不改变驱动传输位深。'))+'</p><p class=digitalFootnote id=rlStudioShaperHint role=status></p><button class=btn type=button data-shaper-help=1>'+esc(t('抖动说明'))+'</button></section>'
 +'<section class=digitalModule data-studio-group=dsd><header><h2>DSD 调制</h2><button class=btn type=button data-studio-advanced=sdm>高级</button></header><div class=digitalFields>'
 +rlStudioField('rlStudioSdmFilter1','SDM 1x 滤波',rlStudioSdmFilterOptions('sFilt','跟随 PCM 1x'),true,'sdmfilt')+rlStudioField('rlStudioSdmFilterNx','SDM Nx 滤波',rlStudioSdmFilterOptions('sFiltNx','跟随 PCM Nx'),true,'sdmfiltnx')+rlStudioField('rlStudioModFamily','调制器族',[['dsd','DSD 标准'],['chebyshev','Chebyshev'],['ec','EC'],['ahm','AHM']].map(function(p){return rlStudioOption(p[0],p[1]);}).join(''),false,'modfamily')+rlStudioField('rlStudioModOrder','请求阶数',rlStudioOption(5,'5 阶')+rlStudioOption(7,'7 阶'),false,'modorder')+'</div><p class=digitalReading id=rlStudioReading_dsd></p></section>'
 +'<section class=digitalModule data-studio-group=output><header><h2>输出规格</h2><button class=btn type=button data-studio-advanced=output>高级</button></header><div class=digitalFields>'+rlStudioField('rlStudioOutputMode','输出格式',rlStudioOption('pcm','PCM')+rlStudioOption('dop','DSD · DoP')+rlStudioOption('native','DSD · Native'),true,'outputMode')+rlStudioField('rlStudioPcmRate','PCM 采样率','',true,'rate')+rlStudioField('rlStudioRateFamily','采样率族','',true,'rateFamilyPolicy')+rlStudioField('rlStudioDsdRate','DSD 倍率',[64,128,256,512,1024].map(function(n){return rlStudioOption(n,'DSD'+n);}).join(''),true,'dsdRate')+'</div><p class=digitalReading id=rlStudioReading_output></p></section></div>'
 +'<div class=digitalQuickEq><label for=rlStudioQuickEq><span>加载 EQ</span><select id=rlStudioQuickEq aria-label="加载已保存的均衡"></select></label><button class=btn type=button data-studio-tab=eq>打开调音台</button><span id=rlStudioQuickEqState class=digitalTag></span></div><p class=digitalFootnote>从 16 个已保存均衡中选择一个载入；选择「不载入（关闭均衡）」会关闭当前组均衡并清空关联。没有确认键，所有选项即时生效。双击组名可改名；音量和输出设备独立于 A–D。</p>'
 +'<section class=digitalSegment id=rlStudioSegment><header class=digitalSectionHeading><h2>同曲片段</h2><div class=rlSegQuick><label class=digitalCheck><input type=checkbox id=rlStudioLoop>循环此片段</label><button class=btn type=button id=rlStudioSegClear>清除片段</button></div></header><div class=rlSegWaveWrap><div id=rlStudioWaveform class="digitalWaveform rlSegWave"></div><div class=rlSegReadout><span class=rlSegLead>拖动选取片段 · 选区内拖动整体平移</span><output id=rlStudioRangeReadout>—</output></div></div><div class=digitalSegmentTools hidden><label>起点 · 秒<input id=rlStudioRangeStart type=number min=0 step=.01></label><button class=btn type=button id=rlStudioSetStart>设为当前位置</button><label>终点 · 秒<input id=rlStudioRangeEnd type=number min=0 step=.01></label><button class=btn type=button id=rlStudioSetEnd>设为当前位置</button></div><p class=rlSegHint id=rlStudioRangeState>在波形空白处拖动可选取新片段；在选区内按住拖动可整段平移；拖两端微调。拖动时播放中会跟手跳到该位置试听，松手自动保存。</p></section></section>'
 +'<section id=rlStudioPanel_eq class=digitalPanel role=tabpanel aria-labelledby=rlStudioTab_eq hidden>'+(typeof rlEqConsoleHTML==='function'?rlEqConsoleHTML():'')+'</section>'
 +'<section id=rlStudioPanel_monitor class=digitalPanel role=tabpanel aria-labelledby=rlStudioTab_monitor hidden><header class=digitalMonitorHeader><div><h2 id=rlStudioMonitorTrack></h2><p id=rlStudioMonitorMeta></p></div><span class=digitalTag>当前曲目</span></header>'+(typeof rlStudioMonitorHTML==='function'?rlStudioMonitorHTML():'')+'</section>'
 +'<section class=digitalQueue><header class=digitalSectionHeading><h2>播放队列</h2><button class=btn type=button id=rlStudioAll>查看全部</button></header><div id=rlStudioQueue></div></section>';
 home.appendChild(studio);D('rlStudioSignalHost').appendChild(path);
 ['rlStudioSource','rlStudioFilter','rlStudioOutput'].forEach(function(id,index){var node=path.querySelectorAll('.rlPathNode')[index];if(node&&!D(id)){var span=document.createElement('span');span.id=id;span.className='digitalSignalDetail';node.appendChild(span);}});
 studio.addEventListener('change',rlStudioEdit);studio.addEventListener('input',function(e){if(e.target.matches('input[data-studio-key]'))rlStudioEdit(e);});
 studio.addEventListener('click',rlStudioClick);studio.addEventListener('dblclick',function(e){clearTimeout(RL_STUDIO.nameTimer);var name=e.target.closest('[data-slot-name]');if(name)rlStudioRename(name.dataset.slotName);});
 studio.querySelector('.digitalTabs').addEventListener('keydown',function(e){if(e.key!=='ArrowLeft'&&e.key!=='ArrowRight')return;e.preventDefault();var tabs=['ab','eq','monitor'],i=tabs.indexOf(RL_STUDIO.tab);rlStudioTab(tabs[(i+(e.key==='ArrowRight'?1:2))%3]);D('rlStudioTab_'+RL_STUDIO.tab).focus();});
 D('rlStudioAll').onclick=function(){rlNav('queue');};
 D('rlStudioSetStart').onclick=function(){rlStudioSetBoundary('start');};D('rlStudioSetEnd').onclick=function(){rlStudioSetBoundary('end');};
 ['rlStudioRangeStart','rlStudioRangeEnd'].forEach(function(id){D(id).onchange=function(){rlStudioRangeChange(Number(D('rlStudioRangeStart').value),Number(D('rlStudioRangeEnd').value),true,true);};});
 D('rlStudioLoop').onchange=function(){rlStudioRangeChange(Number(D('rlStudioRangeStart').value),Number(D('rlStudioRangeEnd').value),true,false);};
 D('rlStudioSegClear').onclick=function(){rlStudioClearRange();};
 D('rlStudioQueue').onclick=function(e){var b=e.target.closest('button[data-index]'),q=S.q;if(!b||!q||!PLAYBACK_SETTINGS.ready)return;var i=Number(b.dataset.index),cue=(q.cues||[])[i]||[-1,-1];if(q.items[i])playPath(q.items[i],cue[0],cue[1],i);};
 rlStudioSubscribe();if(typeof rlAttachEqConsole==='function')rlAttachEqConsole();if(typeof rlAttachStudioMonitor==='function')rlAttachStudioMonitor();applyI18n(studio);
 Promise.all([rlCompare.refresh(),rlConsole.refresh()]).then(function(){RL_STUDIO.selected=rlCompare.activeId;rlStudioSync();}).catch(function(e){rlStudioSay(e.message,true);});rlStudioSync();
}
function rlAttachStudio(){return rlInstallStudio();}
function rlStudioSubscribe(){if(RL_STUDIO.subscribed)return;rlConsole.subscribe(rlStudioSync);rlCompare.subscribe(rlStudioSync);RL_STUDIO.subscribed=true;}
function rlStudioSay(message,error){var e=D('rlStudioMessage');if(e){e.textContent=t(message||'');e.dataset.error=String(!!error);}}
function rlStudioTab(tab){if(!['ab','eq','monitor'].includes(tab))return;RL_STUDIO.tab=tab;['ab','eq','monitor'].forEach(function(id){D('rlStudioPanel_'+id).hidden=id!==tab;var b=D('rlStudioTab_'+id);b.setAttribute('aria-selected',String(id===tab));b.tabIndex=id===tab?0:-1;});if(tab==='eq'&&typeof rlEqRefresh==='function')rlEqRefresh();if(tab==='monitor'&&typeof rlPaintStudioMonitor==='function')rlPaintStudioMonitor(RL_STUDIO.frame);if(tab==='ab')rlStudioWave();}
function rlOpenStudioModule(group){if(!D('rlStudio'))rlInstallStudio();if(group==='eq'){rlStudioTab('eq');return;}rlStudioTab('ab');if(group==='pcm'||group==='sdm'||group==='output')rlStudioAdvanced(group);}
function rlStudioClick(e){var b=e.target.closest('button');if(!b)return;if(b.dataset.shaperHelp){rlOpenShaperInfo();return;}if(b.dataset.filterBrowse){rlFilterBrowserOpen(b.dataset.filterBrowse);return;}if(b.dataset.studioTab)rlStudioTab(b.dataset.studioTab);if(b.dataset.studioSlot&&!e.target.closest('[data-slot-name] input')){var name=e.target.closest('[data-slot-name]');if(name){clearTimeout(RL_STUDIO.nameTimer);RL_STUDIO.nameTimer=setTimeout(function(){rlStudioSelect(b.dataset.studioSlot);},280);}else rlStudioSelect(b.dataset.studioSlot);}if(b.dataset.studioAdvanced)rlStudioAdvanced(b.dataset.studioAdvanced);}
function rlStudioRun(action,message){
 RL_STUDIO.pending=true;rlStudioSay('应用中…');rlStudioSync();
 return rlWorkbenchQueue.run(action).then(function(result){rlStudioSay(message||'已自动保存并应用');return result;}).catch(function(e){rlStudioSay(e.message||'应用失败，已保留原配置',true);throw e;}).finally(function(){RL_STUDIO.pending=rlWorkbenchQueue.pending>0;rlStudioSync();});
}
function rlStudioEdit(e){if(RL_STUDIO.switching)return;var field=e.target,key=field.dataset.studioKey;if(!key||field.disabled)return;
 var value=field.type==='checkbox'?(field.checked?'1':'0'):field.value;
 if(field.type==='number'&&(!field.checkValidity()||!Number.isFinite(Number(value)))){rlStudioSay('请输入范围内的有效数值',true);return;}
 if(key==='outputMode'||key==='dsdRate'){var mode=D('rlStudioOutputMode').value;RL_STUDIO.draft.dsd=mode==='pcm'?'0':D('rlStudioDsdRate').value;RL_STUDIO.draft.dsdnative=mode==='native'?'1':'0';}
 else RL_STUDIO.draft[key]=value;
 RL_STUDIO.editVersion++;rlStudioOutputVisibility();if(key.indexOf('cleanup')===0)rlStudioSync();rlStudioSay('正在自动保存…');clearTimeout(RL_STUDIO.timer);RL_STUDIO.timer=setTimeout(function(){rlStudioFlush().catch(function(){});},field.type==='number'||field.type==='text'?450:150);
}
function rlStudioFlush(){
 clearTimeout(RL_STUDIO.timer);
 // 秒切是乐观的：本地先画 B，后台还在确认 select。此时立刻编辑不能报“正在切换”，
 // 而是等 select 落定后再把编辑排进同一队列。
 if(typeof rlCompare!=='undefined'&&rlCompare.saving&&RL_STUDIO.instantSelect){
  return new Promise(function(resolve,reject){var waited=0,timer=setInterval(function(){waited+=25;if(!rlCompare.saving){clearInterval(timer);resolve();}else if(waited>=10000){clearInterval(timer);reject(Error(t('正在切换，请稍候')));}},25);}).then(rlStudioFlush);
 }var rate=D('rlStudioPcmRate'),choice=rate&&rate.selectedOptions[0];if(Object.hasOwn(RL_STUDIO.draft,'rate')&&D('rlStudioOutputMode').value==='pcm'&&choice&&choice.disabled){rlStudioSay('当前设备不支持所选采样率，请选择可用规格',true);return Promise.reject(Error(t('当前设备不支持所选采样率，请选择可用规格'))); }var keys=Object.keys(RL_STUDIO.draft);if(!keys.length)return Promise.resolve();
 var patch=Object.assign({},RL_STUDIO.draft),id=rlCompare.activeId;RL_STUDIO.draft={};Object.assign(RL_STUDIO.inflight,patch);
 return rlStudioRun(function(){return rlCompare.update(patch,id);}).finally(function(){Object.keys(patch).forEach(function(k){if(RL_STUDIO.inflight[k]===patch[k])delete RL_STUDIO.inflight[k];});rlStudioSync();});
}
function rlStudioWaitSelect(){
 if(typeof rlCompare==='undefined'||!rlCompare.saving)return Promise.resolve();
 return new Promise(function(resolve,reject){var waited=0,timer=setInterval(function(){waited+=25;if(!rlCompare.saving){clearInterval(timer);resolve();}else if(waited>=10000){clearInterval(timer);reject(Error(t('正在切换，请稍候')));}},25);});
}
function rlStudioApplyLegacySettings(settings){
 if(!settings||typeof SET_KEYS==='undefined')return;
 SET_KEYS.forEach(function(p){var e=D(p[0]);if(!e||settings[p[1]]===undefined)return;if(e.tagName==='SELECT'&&typeof setSelectValue==='function')setSelectValue(e,String(settings[p[1]]));else e.value=String(settings[p[1]]);});
}
function rlStudioSelect(id){
 if(id===rlCompare.activeId||RL_STUDIO.switching)return Promise.resolve();
 var hasDraft=Object.keys(RL_STUDIO.draft).length>0;
 var eqBusy=typeof RL_EQ_WRITE!=='undefined'&&!!(RL_EQ_WRITE.busy||RL_EQ_WRITE.pending);
 if(rlCompare.saving&&!hasDraft&&!eqBusy)return rlStudioWaitSelect().then(function(){return rlStudioSelect(id);});
 if(hasDraft||eqBusy){
  // 有未落盘编辑时先按旧路径刷新，避免把编辑写到下一组。
  RL_STUDIO.switching=true;rlStudioSync();
  return Promise.all([rlStudioFlush(),typeof rlEqFlush==='function'?rlEqFlush():Promise.resolve()]).then(function(){return rlStudioRun(function(){return rlCompare.load(id);},'已切换当前组');}).catch(function(){}).finally(function(){RL_STUDIO.switching=false;rlStudioSync();});
 }
 var slot=rlCompare.slots.find(function(s){return s.id===id;});
 if(!slot||!slot.settings||!rlConsole.state){
  RL_STUDIO.switching=true;rlStudioSync();
  return rlStudioRun(function(){return rlCompare.load(id);},'已切换当前组').catch(function(){}).finally(function(){RL_STUDIO.switching=false;rlStudioSync();});
 }
 var previous={id:rlCompare.activeId,settings:JSON.parse(JSON.stringify(rlConsole.state.settings||{})),peq:JSON.parse(JSON.stringify(rlConsole.state.peq||null))};
 // 乐观秒切：先用本地快照把四个槽位、参数和 EQ 一起画出来，再让服务端确认。
 rlCompare.activeId=id;
 rlConsole.state.settings=Object.assign({},rlConsole.state.settings||{},slot.settings||{});
 if(slot.peq)rlConsole.state.peq=JSON.parse(JSON.stringify(slot.peq));
 rlConsole.state.profile=id;rlStudioApplyLegacySettings(slot.settings);
 rlCompare.notify();rlStudioSync();
 if(typeof rlEqSync==='function')rlEqSync(rlConsole.state,false);
 rlStudioSay(tf('已切换到 {0} 组',[id]));
 RL_STUDIO.instantSelect=true;
 return rlCompare.load(id,true).then(function(result){
  // 后台用服务端状态校准一次，不阻塞点击；失败时回滚到切换前的快照。
  setTimeout(function(){if(typeof rlConsole!=='undefined')rlConsole.refresh(true).catch(function(){});},260);
  return result;
 }).catch(function(e){
  if(rlCompare.activeId===id){
   rlCompare.activeId=previous.id;
   rlConsole.state.settings=previous.settings;
   if(previous.peq)rlConsole.state.peq=previous.peq;
   rlCompare.notify();rlStudioSync();
   if(typeof rlEqSync==='function')rlEqSync(rlConsole.state,false);
  }
  rlStudioSay(e.message||t('切换失败'),true);throw e;
 }).finally(function(){RL_STUDIO.instantSelect=false;});
}
function rlStudioSelectDelta(delta){var ids=['A','B','C','D'],i=ids.indexOf(rlCompare.activeId);return rlStudioSelect(ids[(Math.max(0,i)+delta+4)%4]);}
function rlStudioRename(id){
 if(RL_STUDIO.rename)return;var span=document.querySelector('[data-slot-name="'+id+'"]'),slot=rlCompare.slots.find(function(s){return s.id===id;});if(!span||!slot)return;
 RL_STUDIO.rename=id;var input=document.createElement('input');input.value=slot.name||id;input.maxLength=48;input.setAttribute('aria-label',t('对比组名称'));span.textContent='';span.appendChild(input);input.focus();input.select();var done=false;
 function finish(save){if(done)return;done=true;var name=input.value.trim();RL_STUDIO.rename=null;if(save&&name&&name!==slot.name)rlStudioRun(function(){return rlCompare.rename(id,name);},'组名已保存').catch(function(){});else rlStudioPaintCompare(true);}
 input.addEventListener('click',function(e){e.stopPropagation();});input.addEventListener('dblclick',function(e){e.stopPropagation();});input.onkeydown=function(e){e.stopPropagation();if(e.key==='Enter'){e.preventDefault();finish(true);}if(e.key==='Escape'){e.preventDefault();finish(false);}};input.onblur=function(){finish(true);};
}
// 2026-11-15（用户要求）：A–D 四组各自绑定输出后端/设备，方便 AB 两台设备。
// 口径：backend/device/buffer 已进入配置组（workbench_profiles::profile_key），
// 切组时由工作台按同一播放位置重开流；这里只负责把选择写进当前组。
var RL_STUDIO_ROUTE = { names: {}, loading: 0 };
function rlStudioBackendLabel(v){return t(({wasapi:'WASAPI 独占', 'wasapi-shared':'WASAPI 共享', asio:'ASIO', null:'Null 不出声', upnp:'UPnP 投放'})[v]||v||'WASAPI 独占');}
function rlStudioRouteName(v){return RL_STUDIO_ROUTE.names[v]||v||t('默认设备');}
function rlStudioRouteDevices(backend,selected){
  if(backend==='null')return Promise.resolve([{id:'',name:t('不出声（只跑 DSP）')}]);
  if(backend==='upnp')return Promise.resolve([{id:'',name:t('使用设置→输出里的投放设备')}]);
  var url=backend==='asio'?'/api/asiodevices':'/api/devices';
  var gen=++RL_STUDIO_ROUTE.loading;
  return api(url).then(function(j){
    var list=(j&&(j.devices||j.drivers))||[];
    list.forEach(function(d){if(d&&d.id)RL_STUDIO_ROUTE.names[d.id]=d.name||d.id;});
    return gen===RL_STUDIO_ROUTE.loading?list:[];
  }).catch(function(){return [];});
}
function rlStudioPaintRouteDevice(sel,list,selected,backend){
  if(!sel)return;
  var def=backend==='asio'?t('（第一个可用驱动）'):(backend==='null'?t('不出声（只跑 DSP）'):(backend==='upnp'?t('使用全局投放设备'):t('默认设备')));
  var h='<option value="">'+esc(def)+'</option>';
  list.forEach(function(d){
    if(!d||!d.id)return;
    var name=d.name||d.id||'';
    if(d.default)name+=' '+t('（默认）');
    if(d.channels)name+=' · '+d.channels+'ch'+(d.mixRate?('/'+(d.mixRate/1000)+'k'):'');
    h+='<option value="'+esc(d.id||'')+'">'+esc(name)+'</option>';
  });
  if(!h)h='<option value="">'+esc(t('（没有可用设备）'))+'</option>';
  sel.innerHTML=h;
  var want=selected||'';
  if(want&&!Array.from(sel.options).some(function(o){return o.value===want;})){
    sel.add(new Option(want,want));
  }
  sel.value=want;
}
function rlStudioOutputRoute(dialog){
  var state=rlStudioCurrent(),s=(state&&state.settings)||{};
  var slot=rlCompare.slots.find(function(x){return x.id===rlCompare.activeId;});
  if(slot&&slot.settings)s=slot.settings;
  var backend=s.backend||ADV.backend||'wasapi',device=s.device||'';
  var wrap=document.createElement('section');
  wrap.className='digitalField wide';
  wrap.innerHTML='<span>'+esc(t('输出后端（本组）'))+'</span>'
    +'<select id=rlStudioRouteBackend data-studio-key="backend">'
    +[['wasapi','WASAPI 独占'],['wasapi-shared','WASAPI 共享'],['asio','ASIO'],['null','Null 不出声']].map(function(p){return '<option value="'+p[0]+'">'+esc(t(p[1]))+'</option>';}).join('')
    +'</select>'
    +'<span>'+esc(t('输出设备（本组）'))+'</span>'
    +'<select id=rlStudioRouteDevice data-studio-key="device"></select>'
    +'<small>'+esc(t('A–D 可各自指向不同后端/设备；切组时按同一播放位置重开流，会有短暂停顿。UPnP 投放仍使用设置→输出里的全局设备。'))+'</small>';
  dialog.querySelector('.digitalFields').appendChild(wrap);
  var bsel=D('rlStudioRouteBackend'),dsel=D('rlStudioRouteDevice');
  bsel.value=backend;
  rlStudioRouteDevices(backend,device).then(function(list){rlStudioPaintRouteDevice(dsel,list,device,backend);});
  bsel.addEventListener('change',function(){
    var b=bsel.value;
    RL_STUDIO.draft.backend=b;
    RL_STUDIO.draft.device='';
    rlStudioRouteDevices(b,'').then(function(list){rlStudioPaintRouteDevice(dsel,list,'',b);});
    rlStudioSay(t('正在切换输出后端…'));
  });
  dsel.addEventListener('change',function(){RL_STUDIO.draft.device=dsel.value;});
}
function rlStudioCrossfeedPreset(name) {
 if(!['off','light','standard'].includes(name)||RL_STUDIO.switching||!PLAYBACK_SETTINGS.ready)return Promise.resolve();
 var patch=name==='off'?{crossfeed:'0'}:{crossfeed:'1',crossfeed_freq:'700',crossfeed_level:name==='light'?'12':'6'};
 Object.assign(RL_STUDIO.draft,patch);RL_STUDIO.editVersion++;
 var dialog=D('rlStudioAdvancedDialog');if(dialog)Object.keys(patch).forEach(function(key){rlStudioSetValue(dialog.querySelector('[data-studio-key="'+key+'"]'),patch[key]);});
 clearTimeout(RL_STUDIO.timer);return rlStudioFlush().catch(function(){});
}
function rlStudioAdvanced(group){var old=D('rlStudioAdvancedDialog');if(old)old.remove();var dialog=document.createElement('dialog');dialog.id='rlStudioAdvancedDialog';dialog.className='digitalAdvanced';dialog.dataset.studioAdvancedGroup=group;var titles={pcm:'PCM 高级参数',sdm:'DSD 高级参数',output:'输出与处理高级参数'};var schema=RL_STUDIO_ADVANCED[group]||[];
 var nativeClose=dialog.close.bind(dialog);
 dialog.close=function(){try{nativeClose();}finally{if(dialog.parentNode)dialog.remove();}};
 dialog.innerHTML='<header><h2>'+esc(titles[group])+'</h2><button class=btn type=button aria-label="关闭">×</button></header><div class=digitalFields>'+schema.map(function(f){var options='';if(f[2]==='select')options=f[3].map(function(o){return '<option value="'+esc(o[0])+'">'+esc(o[1])+'</option>';}).join('');if(f[2]==='filter')options='<option value="">跟随上级滤波</option>'+(D('sFilt')?D('sFilt').innerHTML:'');var control=f[2]==='select'||f[2]==='filter'?'<select data-studio-key="'+f[0]+'">'+options+'</select>':'<input data-studio-key="'+f[0]+'" type="'+(f[2]==='check'?'checkbox':f[2])+'"'+(f[2]==='number'?' min="'+f[3][0]+'" max="'+f[3][1]+'" step="'+f[3][2]+'"':'')+'>';return '<label class="digitalField'+(f[2]==='text'?' wide':'')+'"><span>'+esc(f[1])+'</span>'+control+'<small>'+esc(f[4])+'</small></label>';}).join('')+'</div><footer><span>改动自动保存并应用到当前组</span><span id=rlStudioAdvancedState></span></footer>';
 if(group==='pcm'){
  var presets=document.createElement('div');presets.className='digitalField wide';
  presets.innerHTML='<span>'+esc(t('耳机交叉馈送'))+'</span><div style="display:flex;gap:8px;flex-wrap:wrap">'+[['off','关闭'],['light','轻度 · 12 dB'],['standard','标准 · 6 dB']].map(function(p){return '<button class=btn type=button data-crossfeed-preset="'+p[0]+'">'+esc(t(p[1]))+'</button>';}).join('')+'</div>';
  dialog.querySelector('.digitalFields').prepend(presets);
  presets.querySelectorAll('button').forEach(function(button){button.onclick=function(){rlStudioCrossfeedPreset(button.dataset.crossfeedPreset);};});
 }
 if(group==='pcm'){var status=document.createElement('section');status.className='digitalField wide';status.innerHTML='<span>'+esc('清理实况')+'</span><p id=rlStudioCleanupStatus role=status></p><button class=btn type=button id=rlStudioCleanupRefresh>'+esc('刷新清理状态')+'</button>';dialog.querySelector('.digitalFields').appendChild(status);D('rlStudio').appendChild(dialog);D('rlStudioCleanupRefresh').onclick=rlStudioRefreshCleanupStatus;}else D('rlStudio').appendChild(dialog);
 if(group==='output')rlStudioOutputRoute(dialog);dialog.querySelector('header button').onclick=function(){dialog.close();};dialog.addEventListener('click',function(e){if(e.target!==dialog)return;var r=dialog.getBoundingClientRect();if(e.clientX<r.left||e.clientX>r.right||e.clientY<r.top||e.clientY>r.bottom)dialog.close();});dialog.addEventListener('close',function(){RL_STUDIO.cleanupGeneration=(RL_STUDIO.cleanupGeneration||0)+1;rlStudioFlush().catch(function(){});dialog.remove();});rlStudioSync();applyI18n(dialog);dialog.showModal();if(group==='pcm')rlStudioRefreshCleanupStatus();
}
function rlStudioCleanupConfig(){var state=rlStudioCurrent(),kv=Object.assign({},state&&state.settings,RL_STUDIO.inflight,RL_STUDIO.draft);return {enabled:rlStudioEnabled(kv.cleanup),auto:kv.cleanup_auto===undefined||rlStudioEnabled(kv.cleanup_auto),manualNoiseDb:Number(kv.cleanup_noise_db),backend:kv.backend||ADV.backend};}
function rlStudioPaintCleanupStatus(){var node=D('rlStudioCleanupStatus');if(!node||typeof cleanupStatusText!=='function')return;var c=rlStudioCleanupConfig(),state=rlStudioCurrent()||{},source=c.backend==='upnp'?'cast':'local',report=RL_STUDIO.cleanupReport,facts=report&&report.source===source&&report.identity===rlStudioTrackIdentity()?report.facts:null;node.textContent=cleanupStatusText(facts,{enabled:c.enabled,auto:c.auto,manualNoiseDb:c.manualNoiseDb,playing:source==='cast'?!!(facts&&facts.running):!!state.playing,paused:source==='cast'?false:!!state.paused,current:source==='cast'?facts&&(facts.src||facts.file)||'':state.file||''});}
function rlStudioRefreshCleanupStatus(){
 rlStudioPaintCleanupStatus();var dialog=D('rlStudioAdvancedDialog'),c=rlStudioCleanupConfig();if(!dialog||!dialog.open||dialog.dataset.studioAdvancedGroup!=='pcm'||!c.enabled||rlMode!=='pro'||S.view!=='listening')return Promise.resolve();
 var source=c.backend==='upnp'?'cast':'local',identity=rlStudioTrackIdentity(),generation=RL_STUDIO.cleanupGeneration=(RL_STUDIO.cleanupGeneration||0)+1;
 function current(){return generation===RL_STUDIO.cleanupGeneration&&D('rlStudioAdvancedDialog')===dialog&&dialog.open&&rlMode==='pro'&&S.view==='listening'&&identity===rlStudioTrackIdentity()&&source===(rlStudioCleanupConfig().backend==='upnp'?'cast':'local');}
 return apiGetRaw(source==='cast'?'/api/cast/state':'/api/signal').then(function(j){if(!current())return;RL_STUDIO.cleanupReport={source:source,identity:identity,facts:j&&j.ok?(source==='cast'?j:j.path):null};rlStudioPaintCleanupStatus();}).catch(function(){if(current()){RL_STUDIO.cleanupReport=null;rlStudioPaintCleanupStatus();}});
}
function rlStudioSetValue(el,value){if(!el)return;var v=String(value===undefined?'':value);if(el.type==='checkbox'){el.checked=rlStudioEnabled(v);return;}if(el.tagName==='SELECT'&&!Array.from(el.options).some(function(o){return o.value===v;}))el.add(new Option(v||t('未指定'),v));el.value=v;}
function rlStudioSyncOptions(){[['rlStudioFilter1','sFilt',''],['rlStudioFilterNx','sFiltNx',''],['rlStudioPcmRate','sRate',''],['rlStudioRateFamily','sRateFamilyPolicy',''],['rlStudioTwoStage','s2s',''],['rlStudioShaper','sShap','']].forEach(function(p){
 var target=D(p[0]),source=D(p[1]);if(!target||!source)return;
 // 3.3.9：1x 的“空值”在后端等同自动选型；旧实现没有空值选项，于是显示成「未指定」，
  // 看起来像没配置。这里补一个 value="" 的「自动（推荐）」，与后端语义一致。
 var html=source.innerHTML;
 if(p[0]==='rlStudioFilter1'&&html.indexOf('value=""')<0)html='<option value="">'+esc(t('自动（推荐）'))+'</option>'+html;
 if(target.rlSource===html)return;var value=target.value;target.innerHTML=html;target.rlSource=html;if(Array.from(target.options).some(function(o){return o.value===value;}))target.value=value;
});[['rlStudioSdmFilter1','sFilt','跟随 PCM 1x'],['rlStudioSdmFilterNx','sFiltNx','跟随 PCM Nx']].forEach(function(p){var target=D(p[0]),source=D(p[1]);if(!target||!source)return;var html='<option value="">'+esc(t(p[2]))+'</option>'+source.innerHTML;if(target.rlSource===html)return;var value=target.value;target.innerHTML=html;target.rlSource=html;target.value=value;});}
function rlStudioSync(){
 var studio=D('rlStudio');if(!studio||rlMode!=='pro')return;rlStudioSyncOptions();var state=rlStudioCurrent(),settings=state&&state.settings,busy=RL_STUDIO.pending||rlConsole.busy||rlCompare.saving,ready=!!(settings&&PLAYBACK_SETTINGS.ready&&rlCompare.activeId);
 if(settings){studio.querySelectorAll('[data-studio-key]').forEach(function(field){var key=field.dataset.studioKey;if(Object.hasOwn(RL_STUDIO.draft,key)||Object.hasOwn(RL_STUDIO.inflight,key))return;if(key==='outputMode'||key==='dsdRate'){if(Object.hasOwn(RL_STUDIO.draft,'dsd')||Object.hasOwn(RL_STUDIO.inflight,'dsd'))return;var dsd=String(settings.dsd||'0'),level=Number(dsd.replace(/^(nat|dop)/,''))||0;rlStudioSetValue(field,key==='outputMode'?(level?(rlStudioEnabled(settings.dsdnative)||dsd.indexOf('nat')===0?'native':'dop'):'pcm'):String(level||64));}else rlStudioSetValue(field,settings[key]===undefined?({dacbits:0,crossfeed:0,crossfeed_freq:700,crossfeed_level:12})[key]:settings[key]);});}
 studio.querySelectorAll('[data-studio-key]').forEach(function(field){field.disabled=!ready||RL_STUDIO.switching||(field.dataset.studioKey==='cleanup_noise_db'&&rlStudioCleanupConfig().auto);});rlStudioPaintCleanupStatus();studio.querySelectorAll('[data-studio-slot],#rlStudioComparePrev,#rlStudioCompareNext').forEach(function(b){b.disabled=!ready||RL_STUDIO.switching;});
 var raw=D('rlStudioRawVolumeNotice');raw.hidden=!(typeof isRawVolumeLocked==='function'&&isRawVolumeLocked());if(!raw.hidden&&typeof rawVolumeMessage==='function')raw.textContent=rawVolumeMessage();rlStudioOutputVisibility();rlStudioPaintCompare();rlStudioQuickEq();if(typeof rlEqSync==='function')rlEqSync(state,busy);if(D('rlStudioAdvancedState'))rlSetText('rlStudioAdvancedState',t(busy?'应用中…':'已自动保存'));if(RL_STUDIO.tab==='ab')rlStudioWave();if(typeof rlRefreshShaperGuidance==='function')rlRefreshShaperGuidance();
}
function rlStudioOutputVisibility(){var mode=D('rlStudioOutputMode');if(!mode)return;var pcm=mode.value==='pcm';D('rlStudioDsdRate').closest('label').hidden=pcm;D('rlStudioPcmRate').closest('label').hidden=!pcm;D('rlStudioRateFamily').closest('label').hidden=!pcm;}
function rlStudioSlotSummary(slot){var s=slot&&slot.settings;if(!s)return t('正在读取…');var level=Number(String(s.dsd||'0').replace(/^(nat|dop)/,''))||0;var route=s.backend?rlStudioBackendLabel(s.backend)+(s.device?' · '+rlStudioRouteName(s.device):''):'';return [route,level?'DSD'+level:s.rate==='src'?t('源采样率'):s.rate==='max'?t('自动采样率'):s.rate+' Hz',s.filter||t('原始滤波')].filter(Boolean).join(' · ');}
// 2026-11-15（用户报「A–D 四个预设名字还是中文」）：A–D 的名字是**用户数据**
//   （存在 rlplayer-workbench-profiles.json 里，双击可改名），默认就是 A/B/C/D，
//   原则上不翻译 —— 跟专辑名、艺人名一样。但旧版本把四组默认名播种成了
//   「流行/女声/男声/古典」，用户从没改过也永远是中文。这里只对**这四个旧默认名**
//   做本地化（词条键带 @group: 前缀，不会撞到界面上的同名文案）；
//   改过名的（任何别的名字）原样显示，绝不碰用户自己起的名字。
var RL_STUDIO_LEGACY_NAMES = { '流行': 1, '女声': 1, '男声': 1, '古典': 1 };
function rlStudioSlotName(slot, id) {
  var nm = (slot && slot.name) || id;
  // 单元测试的 vm 沙箱里没有 I18N（只塞了被测函数需要的全局）⟹ 必须容错，
  // 否则一次取组名就把整页渲染抛断（test_digital_console335 实测）。
  if (typeof I18N === 'undefined' || !I18N || I18N.lang === 'zh' || !RL_STUDIO_LEGACY_NAMES[nm]) return nm;
  var tr = (I18N.map || {})['@group:' + nm];
  return tr || nm;
}
function rlStudioPaintCompare(force){var host=D('rlStudioSlots');if(!host||RL_STUDIO.rename&&!force)return;var html='';['A','B','C','D'].forEach(function(id){var slot=rlCompare.slots.find(function(s){return s.id===id;}),active=rlCompare.activeId===id;html+='<button type=button class=digitalSlot data-studio-slot='+id+((RL_STUDIO.pending||RL_STUDIO.switching)?' disabled':'')+' aria-current='+active+' aria-pressed='+active+'><span class=digitalSlotCode>'+id+'</span><span class=digitalSlotName data-slot-name='+id+' title="'+esc(t('双击改名'))+'">'+esc(rlStudioSlotName(slot,id))+'</span><span class=digitalSlotSettings>'+esc(rlStudioSlotSummary(slot))+'</span><span class=digitalSlotState>'+esc(active?t(RL_STUDIO.pending||rlCompare.saving?'应用中…':rlCompare.error?'应用失败，保留当前组':'当前组 · 自动保存'):t('独立配置'))+'</span></button>';});if(force||host.rlHtml!==html){host.rlHtml=html;host.innerHTML=html;}
 var text=rlCompare.activeId?tf('当前组 {0}',[rlCompare.activeId]):t('正在读取当前组…');if(rlCompare.error)text+=' · '+rlCompare.error;rlSetText('rlStudioCompareState',text);RL_STUDIO.selected=rlCompare.activeId||'A';}
function rlStudioQuickEq(){
 var sel=D('rlStudioQuickEq');if(!sel)return;
 var bank=(typeof RL_EQ!=='undefined'&&RL_EQ.bank||[]).filter(function(s){return s&&s.peq;}),slot=rlCompare.slots.find(function(s){return s.id===rlCompare.activeId;}),active=slot&&slot.eqId||'';
 // 16 个槽位都可能来自耳机校准或自定义，这里不再分两栏；当前组若还有未保存的
 // 手动 PEQ（eqId 为空、bypass=0），单独标出来，避免它被误显示成"未载入"。
 var off=!!(slot&&slot.peq&&Number(slot.peq.bypass)===1),manual=!!(slot&&slot.peq&&!active&&!off);
 var html='<option value="">'+esc(t('不载入（关闭均衡）'))+'</option>';
 if(manual)html+='<option value="__manual__">'+esc(t('未保存的均衡（当前）'))+'</option>';
 bank.forEach(function(s){html+=rlStudioOption(s.id,s.name+' · '+(s.source==='opra'?t('耳机校准'):t('自定义均衡')));});
 if(sel.rlHtml!==html){sel.innerHTML=html;sel.rlHtml=html;}
 var current=bank.find(function(s){return s.id===active;}),want=current?active:(manual?'__manual__':'');
 if(document.activeElement!==sel)sel.value=want;
 sel.onchange=function(){
  var v=sel.value;
  if(!v){
   // 空值必须真的关闭，而不是 return 后继续显示旧预设（用户报的"设置后取消不了"）。
   if(typeof rlEqUnload==='function')rlEqUnload();
   else if(typeof rlEqRestore==='function')rlEqRestore();
   return;
  }
  if(v==='__manual__')return;
  if(typeof rlEqPresetLoad==='function')rlEqPresetLoad(v);
 };
 rlSetText('rlStudioQuickEqState',current?tf('{0} · {1}',[current.source==='opra'?'耳机校准':'自定义均衡',current.name]):off?t('均衡已关闭'):manual?t('当前为未保存的均衡'):t('当前未载入均衡（选择后自动载入）'));
}
function rlStudioTime(value){var v=Math.max(0,Number(value)||0);return fmt(v)+'.'+String(Math.floor(v*100)%100).padStart(2,'0');}
function rlStudioTrackIdentity(){var s=rlStudioCurrent();return s?JSON.stringify([s.file,s.queueIndex,s.cueStart,s.cueEnd]):'';}
function rlStudioLoopMatches(loop,state){return !!state&&loop.file===state.file&&Number(loop.cueStart)===Number(state.cueStart)&&Number(loop.cueEnd)===Number(state.cueEnd);}
function rlStudioWave(){var state=rlStudioCurrent();if(!state||!state.file){if(RL_STUDIO.waveform)RL_STUDIO.waveform.update({file:'',trackKey:'',duration:0,position:0,start:0,end:0});RL_STUDIO.rangeIdentity='';RL_STUDIO.rangeFile='';RL_STUDIO.range={start:0,end:0};D('rlStudioLoop').checked=false;rlStudioRangePaint();return;}var duration=Number(state.duration)||Number(RL_STUDIO.frame&&RL_STUDIO.frame.facts&&RL_STUDIO.frame.facts.duration)||0,identity=rlStudioTrackIdentity();
 if(identity!==RL_STUDIO.rangeIdentity){RL_STUDIO.rangeIdentity=identity;RL_STUDIO.rangeFile=state.file;var loop=rlCompare.loop||{},same=rlStudioLoopMatches(loop,state);RL_STUDIO.range={start:same?Math.min(Number(loop.start)||0,duration):0,end:same&&Number(loop.end)>0?Math.min(Number(loop.end),duration):duration};D('rlStudioLoop').checked=same&&!!loop.enabled&&!!loop.repeat;rlStudioRangePaint();}
 var options={file:state.file,clipStart:Number(state.cueStart)>=0?Number(state.cueStart):0,clipLength:Number(state.cueStart)>=0&&Number(state.cueEnd)>Number(state.cueStart)?Number(state.cueEnd)-Number(state.cueStart):0,trackKey:identity,duration:duration,position:Number((typeof rlLastStatus!=='undefined'&&rlLastStatus&&state&&rlLastStatus.current===state.file&&rlLastStatus.pos)||state.position)||0,start:RL_STUDIO.range.start,end:RL_STUDIO.range.end,selection:true,rangeControls:false,dragSelect:true,onRange:rlStudioRangeChange,onSeek:function(v){apiAct('/api/seek?pos='+encodeURIComponent(duration>0?v/duration:0),'').catch(function(e){rlStudioSay(e.message,true);});}};
 if(!RL_STUDIO.waveform&&typeof rlMountWaveform==='function')RL_STUDIO.waveform=rlMountWaveform(D('rlStudioWaveform'),options);else if(RL_STUDIO.waveform)RL_STUDIO.waveform.update(options);
}
function rlStudioRangePaint(){var range=RL_STUDIO.range;D('rlStudioRangeStart').value=range.start.toFixed(2);D('rlStudioRangeEnd').value=range.end.toFixed(2);rlSetText('rlStudioRangeReadout',rlStudioTime(range.start)+' — '+rlStudioTime(range.end));}
function rlStudioRangeChange(start,end,committed){var state=rlStudioCurrent(),duration=state&&Number(state.duration)||0;
 if(!state||!state.file||rlStudioTrackIdentity()!==RL_STUDIO.rangeIdentity){rlStudioSay('曲目已切换，请重新设置片段',true);return;}
 if(!Number.isFinite(start)||!Number.isFinite(end)||start<0||end<=start||duration>0&&end>duration+.01){rlStudioSay('片段终点必须晚于起点，并位于当前曲目内',true);rlStudioRangePaint();return;}
 RL_STUDIO.range={start:start,end:end};rlStudioRangePaint();if(RL_STUDIO.waveform)RL_STUDIO.waveform.update({start:start,end:end});if(!committed)return;
 var identity=rlStudioTrackIdentity(),loop={file:state.file,start:start,end:end,enabled:1,repeat:D('rlStudioLoop').checked?1:0},version=++RL_STUDIO.rangeVersion;rlSetText('rlStudioRangeState',t('正在保存片段…'));
 rlWorkbenchQueue.run(function(){return rlConsole.refresh().then(function(current){if(rlStudioTrackIdentity()!==identity)throw Error(t('曲目已切换，请重新设置片段'));loop.expected_revision=String(current.revision);return rlCompare.setLoop(loop);});}).then(function(){if(version!==RL_STUDIO.rangeVersion)return;rlSetText('rlStudioRangeState',t(loop.repeat?'片段已保存，循环已启用':'片段已保存，结束时暂停'));}).catch(function(e){rlStudioSay(e.message,true);if(version===RL_STUDIO.rangeVersion){var l=rlCompare.loop||{};if(rlStudioLoopMatches(l,state))RL_STUDIO.range={start:Math.min(Number(l.start)||0,duration),end:Math.min(Number(l.end)||duration,duration)};else RL_STUDIO.range={start:0,end:duration};D('rlStudioLoop').checked=rlStudioLoopMatches(l,state)&&!!l.enabled&&!!l.repeat;rlStudioRangePaint();}});
}
function rlStudioSetBoundary(which){var state=rlStudioCurrent();if(!state)return;var position=Number(state.position)||0;rlStudioRangeChange(which==='start'?position:RL_STUDIO.range.start,which==='end'?position:RL_STUDIO.range.end,true);}

/* 2026-11-12：同曲片段重做（覆盖上面的旧实现）。
   旧版三个叠加问题：① 96px 的 .digitalWaveform 空盒 + 100% 不透明选择带 = 大黑框；
   ② 未播放/停止时 duration=0，起终点按钮被两道静默 gate 吞掉；③ enabled 硬编码 1，清除不了。
   新版：波形始终挂载（空态也有说明），按钮在不可用时置灰并给可见原因，允许暂停中设段，
   失败不再回滚用户刚设的范围；另提供「清除片段」。 */
function rlStudioLiveStatus(state){return (typeof rlLastStatus!=='undefined'&&rlLastStatus&&state&&rlLastStatus.current===state.file)?rlLastStatus:null;}
function rlStudioDuration(state){var live=rlStudioLiveStatus(state);return Number(live&&live.dur)||Number(state&&state.duration)||Number(RL_STUDIO.frame&&RL_STUDIO.frame.facts&&RL_STUDIO.frame.facts.duration)||Number(S&&S.dur)||0;}
function rlStudioSegSay(message,error){var e=D('rlStudioRangeState');if(e){e.textContent=t(message||'');e.dataset.error=String(!!error);}}
function rlStudioWave(){
 var state=rlStudioCurrent(),host=D('rlStudioWaveform');
 if(!host)return;
 if(!state||!state.file){
  RL_STUDIO.rangeIdentity='';RL_STUDIO.rangeFile='';RL_STUDIO.range={start:0,end:0};D('rlStudioLoop').checked=false;
  if(typeof rlMountWaveform==='function'&&!RL_STUDIO.waveform)RL_STUDIO.waveform=rlMountWaveform(host,{file:'',trackKey:'',duration:0,position:0,start:0,end:0,selection:true,rangeControls:false});
  else if(RL_STUDIO.waveform)RL_STUDIO.waveform.update({file:'',trackKey:'',duration:0,position:0,start:0,end:0});
  rlStudioRangePaint();rlStudioSegSay('播放一首曲目后即可设置同曲片段。');return;
 }
 var duration=rlStudioDuration(state),identity=rlStudioTrackIdentity();
 if(identity!==RL_STUDIO.rangeIdentity){
  RL_STUDIO.rangeIdentity=identity;RL_STUDIO.rangeFile=state.file;
  var loop=rlCompare.loop||{},same=rlStudioLoopMatches(loop,state);
  RL_STUDIO.range={start:same?Math.max(0,Math.min(Number(loop.start)||0,duration)):0,end:same&&Number(loop.end)>0?Math.max(0,Math.min(Number(loop.end),duration)):duration};
  D('rlStudioLoop').checked=same&&!!loop.enabled&&!!loop.repeat;rlStudioRangePaint();
 }
 var options={file:state.file,clipStart:Number(state.cueStart)>=0?Number(state.cueStart):0,clipLength:Number(state.cueStart)>=0&&Number(state.cueEnd)>Number(state.cueStart)?Number(state.cueEnd)-Number(state.cueStart):0,trackKey:identity,duration:duration,position:Number((typeof rlLastStatus!=='undefined'&&rlLastStatus&&state&&rlLastStatus.current===state.file&&rlLastStatus.pos)||state.position)||0,start:RL_STUDIO.range.start,end:RL_STUDIO.range.end,selection:true,rangeControls:false,dragSelect:true,
  onRange:function(a,b,committed){rlStudioRangeChange(a,b,committed);},
  // 3.3.9：拖手柄/平移片段时跟手跳转，播放中即可听到该位置的声音。
  onScrub:function(v){if(!(duration>0))return;apiAct('/api/seek?pos='+encodeURIComponent(Math.max(0,Math.min(.9999,v/duration))),'').catch(function(){});},
  onSeek:function(v){if(!(duration>0))return;apiAct('/api/seek?pos='+encodeURIComponent(Math.max(0,Math.min(.9999,v/duration))),'').catch(function(e){rlStudioSegSay(e.message,true);});}};
 if(!RL_STUDIO.waveform&&typeof rlMountWaveform==='function')RL_STUDIO.waveform=rlMountWaveform(host,options);else if(RL_STUDIO.waveform)RL_STUDIO.waveform.update(options);
 // 3.3.9：判据要用实时状态（rlLastStatus / S），rlConsole.state 会滞后一拍，
 // 旧写法在已经播放时仍显示“播放或暂停后即可设置片段”，看起来像功能没生效。
 var live=rlStudioLiveStatus(state),playingNow=!!(state.playing||state.paused||(live&&live.playing)||(live&&live.paused)||(typeof S!=='undefined'&&S&&(S.playing||S.paused)));
 if(!playingNow||duration<=0)rlStudioSegSay('播放或暂停后即可设置片段；停止状态下没有可用的曲目时长。');
 else rlStudioSegSay('拖动空白处选取新片段；在选区内拖动可整段平移；拖两端微调。松手自动保存。');
}
function rlStudioRangePaint(){
 var state=rlStudioCurrent(),duration=rlStudioDuration(state),range=RL_STUDIO.range||{start:0,end:0};
 var ready=!!(state&&state.file&&duration>0&&(state.playing||state.paused));
 [['rlStudioRangeStart','start'],['rlStudioRangeEnd','end']].forEach(function(p){var e=D(p[0]);if(!e)return;e.max=duration||0;if(document.activeElement!==e)e.value=(p[1]==='start'?range.start:range.end).toFixed(2);});
 ['rlStudioSetStart','rlStudioSetEnd'].forEach(function(id){var e=D(id);if(e)e.disabled=!ready;});
 var clear=D('rlStudioSegClear');if(clear)clear.disabled=!(state&&state.file);
 rlSetText('rlStudioRangeReadout',ready?rlStudioTime(range.start)+' — '+rlStudioTime(range.end):'—');
}
function rlStudioRangeChange(start,end,committed,autoEnable){
 var state=rlStudioCurrent(),duration=rlStudioDuration(state);
 if(!state||!state.file||!(state.playing||state.paused)||duration<=0){rlStudioSegSay('请先播放（或暂停）一首本地曲目，再设置片段。',true);rlStudioRangePaint();return;}
 var identity=rlStudioTrackIdentity();
 if(RL_STUDIO.rangeIdentity&&identity!==RL_STUDIO.rangeIdentity){rlStudioSegSay('曲目已切换，请重新设置片段。',true);return;}
 start=Math.max(0,Math.min(Number(start)||0,duration));end=Math.max(0,Math.min(Number(end)||0,duration));
 if(end<=start+0.05){rlStudioSegSay('终点必须晚于起点；请先把播放位置移到更靠后的位置，或直接输入终点秒数。',true);rlStudioRangePaint();return;}
 RL_STUDIO.range={start:start,end:end};rlStudioRangePaint();if(RL_STUDIO.waveform)RL_STUDIO.waveform.update({start:start,end:end});if(!committed)return;
 if(autoEnable!==false)D('rlStudioLoop').checked=true;
 var loop={file:state.file,start:start,end:end,enabled:1,repeat:D('rlStudioLoop').checked?1:0},version=++RL_STUDIO.rangeVersion;
 rlStudioSegSay('正在保存片段…');
 function send(retry){
  return rlConsole.refresh(true).then(function(current){
   if(rlStudioTrackIdentity()!==identity)throw Error(t('曲目已切换，请重新设置片段'));
   if(!(current.playing||current.paused)||!(Number(current.duration)>0))throw Error(t('播放已停止，片段未保存'));
   loop.expected_revision=String(current.revision);
   return rlCompare.setLoop(loop).catch(function(e){
    if(!retry&&/409|改变|刷新/.test(String(e&&e.message||'')))return rlConsole.refresh(true).then(function(c){loop.expected_revision=String(c.revision);return rlCompare.setLoop(loop);});
    throw e;
   });
  });
 }
 rlWorkbenchQueue.run(function(){return send(false);}).then(function(){
  if(version!==RL_STUDIO.rangeVersion)return;
  rlStudioSegSay(loop.repeat?'片段已保存并循环播放':'片段已保存（播到终点暂停）');
 }).catch(function(e){
  rlStudioSegSay('片段保存失败：'+(e&&e.message||'未知原因')+'。设置仍保留在界面上，可再点一次。',true);rlStudioSay(e.message,true);
 });
}
function rlStudioSetBoundary(which){
 var state=rlStudioCurrent(),duration=rlStudioDuration(state);
 if(!state||!state.file||!(state.playing||state.paused)||duration<=0){rlStudioSegSay('播放或暂停一首曲目后才能设起点/终点。',true);return;}
 var live=rlStudioLiveStatus(state),position=Math.max(0,Math.min(Number(live&&live.pos)||Number(S&&S.pos)||Number(state.position)||0,duration));
 if(which==='start'){
  var end=Number(RL_STUDIO.range&&RL_STUDIO.range.end)||duration;
  if(end<=position+0.05)end=duration;
  rlStudioRangeChange(position,end,true,true);
 }else{
  if(position<=0.05){rlStudioSegSay('当前位置在曲目开头，无法作为终点。请先播放/拖动到后面的位置。',true);return;}
  var start=Number(RL_STUDIO.range&&RL_STUDIO.range.start)||0;
  if(start>=position-0.05)start=0;
  rlStudioRangeChange(start,position,true,true);
 }
}
function rlStudioClearRange(){
 var state=rlStudioCurrent();
 if(!state||!state.file){rlStudioSegSay('当前没有可清除的片段。');return;}
 var version=++RL_STUDIO.rangeVersion;
 rlStudioSegSay('正在清除片段…');
 rlWorkbenchQueue.run(function(){return rlConsole.refresh(true).then(function(current){return rlCompare.setLoop({enabled:0,file:state.file,expected_revision:String(current.revision)});});}).then(function(){
  if(version!==RL_STUDIO.rangeVersion)return;
  var duration=rlStudioDuration(state);RL_STUDIO.range={start:0,end:duration};D('rlStudioLoop').checked=false;rlStudioRangePaint();if(RL_STUDIO.waveform)RL_STUDIO.waveform.update({start:0,end:duration});rlStudioSegSay('片段已清除；播放恢复整曲。');
 }).catch(function(e){rlStudioSegSay('清除失败：'+(e&&e.message||'未知原因'),true);});
}
function rlPaintStudio(frame){var studio=D('rlStudio');if(!studio)return;RL_STUDIO.frame=frame;studio.hidden=rlMode!=='pro';if(studio.hidden)return;rlSetText('rlStudioState',t(frame.status));D('rlStudioState').dataset.state=S.playing&&!S.paused?'running':'idle';rlSetText('rlStudioTrack',frame.title);var meta=frame.m?[frame.m.artist,frame.m.album||frame.m.folderName].filter(Boolean).join(' · '):(frame.file?parentFolder(frame.file):'');rlSetText('rlStudioMeta',meta);rlSetText('rlStudioMonitorTrack',frame.title);rlSetText('rlStudioMonitorMeta',meta);var cover=D('rlStudioCover');if(cover.dataset.image!==frame.bg){cover.dataset.image=frame.bg;cover.style.backgroundImage=frame.bg;}rlStudioSync();rlStudioSignal(studio,frame.facts);rlPaintStudioQueue();rlStudioWave();if(typeof rlPaintStudioMonitor==='function')rlPaintStudioMonitor(frame);}
function rlStudioSignal(studio,facts){var current=rlStudioCurrent(),key=facts?JSON.stringify([facts.current,facts.outRate,facts.outBits,facts.outMode,current&&current.revision]):'';if(studio.signalKey!==key){studio.signalKey=key;studio.readingAt=0;['rlStudioSource','rlStudioFilter','rlStudioOutput'].forEach(function(id){rlSetText(id,'');});['filter','output','dsd'].forEach(function(id){rlSetText('rlStudioReading_'+id,t(key?'等候实际信号':'等候播放'));});}if(!key||Date.now()-studio.readingAt<2000)return;studio.readingAt=Date.now();api('/api/signal').then(function(j){var s=j&&j.ok&&j.path;if(!studio.isConnected||studio.signalKey!==key||!s||s.src!==facts.current)return;rlSetText('rlStudioSource',[s.codec&&String(s.codec).toUpperCase(),s.inBits>0&&s.inBits+' bit'].filter(Boolean).join(' · '));rlSetText('rlStudioFilter',s.resample&&s.filter?s.filter:'');rlSetText('rlStudioOutput',s.device||'');rlSetText('rlStudioReading_filter',s.resample&&s.filter?s.filter:t('当前未重采样'));rlSetText('rlStudioReading_output',[s.outRate>0?s.outRate+' Hz':'',s.outBits>0?s.outBits+' bit':''].filter(Boolean).join(' / '));rlSetText('rlStudioReading_dsd',s.outMode==='dsd'?t('DSD 输出'):t('当前输出 PCM'));}).catch(function(){});}
function rlPaintStudioQueue(){var host=D('rlStudioQueue'),q=S.q;if(!host||rlMode!=='pro'||!q)return;var items=q.items||[],current=Math.max(0,Math.min(q.index||0,items.length-1)),start=Math.max(0,Math.min(current,items.length-4)),html='';for(var i=start;i<Math.min(start+4,items.length);i++){var file=items[i],cue=(q.cues||[])[i]||[],title=nam(file);if(cue[0]>=0)title+=' · '+fmt(cue[0]);var parts=file.split(/[\\/]/).pop().split('.'),ext=parts.length>1?parts.pop().toUpperCase():'';html+='<button type=button class=digitalQueueRow data-index='+i+(i===current?' aria-current=true':'')+'><span>'+String(i+1).padStart(2,'0')+'</span><span>'+esc(title)+'</span><small>'+esc(ext)+'</small></button>';}if(!html)html='<p class=rlQueueEmpty>'+esc(t('队列为空'))+'</p>';if(host.rlHtml!==html){host.rlHtml=html;host.innerHTML=html;}}
)STUDIO";}
} // namespace rta::net
