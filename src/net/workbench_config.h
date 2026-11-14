#pragma once

// Included after rtaweb's playback and persistence helpers. The same session
// mutex owns ordinary playback and workbench changes, including A/B requests.
namespace workbench {

using Values = std::map<std::string, std::string>;
inline bool sync_active_locked();

inline const std::set<std::string>& audio_keys() {
    static const std::set<std::string> keys = {
        "filter", "filterNx", "rate", "rateFamilyPolicy", "dsd", "dsdnative", "directsdm",
        "shaper", "twostage", "taps", "atten", "transition", "phase", "modorder", "modfamily",
        "modcut", "moddith", "oobgain", "ahmbits", "sdmfilt", "sdmfiltnx", "isp", "eq", "ir",
        "irgpu", "roomcorrauto", "loudness", "rgmode", "rgclip", "cleanup", "cleanup_auto",
        "cleanup_strength", "cleanup_noise_db", "dsdkind", "dsdconv", "dsd2stage", "bits",
        "channels", "trim", "backend", "device", "buffer", "gpuvendor", "upnpdev"
    };
    return keys;
}

inline std::string number(double value) {
    std::ostringstream out;
    out.precision(15);
    out << value;
    return out.str();
}

inline std::string form(const Values& values) {
    std::string body;
    for (const auto& [key, value] : values) {
        if (!body.empty()) body += '&';
        body += artistimg_pct(key) + '=' + artistimg_pct(value);
    }
    return body;
}

inline Values saved_values() {
    Values values;
    std::ifstream in(fs::u8path(path_in_exe_dir("rlplayer-settings.conf")), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto split = line.find('=');
        if (split != std::string::npos && !line.empty() && line[0] != '#')
            values[line.substr(0, split)] = line.substr(split + 1);
    }
    return values;
}

inline Values parameters(const PlayParams& p) {
    std::string trims;
    for (double trim : p.trim) { if (!trims.empty()) trims += ','; trims += number(trim); }
    return {
        {"filter",p.filter},{"filterNx",p.filterNx},{"rate",p.rate.empty()?"src":p.rate},
        {"rateFamilyPolicy",p.rateFamilyPolicy},{"dsd",std::to_string(p.dsd)},
        {"dsdnative",p.dsdNative?"1":"0"},{"directsdm",p.directSdm?"1":"0"},
        {"shaper",p.shaper},{"twostage",p.twoStage<0?"auto":std::to_string(p.twoStage)},
        {"taps",std::to_string(p.taps)},{"atten",number(p.atten)},{"transition",number(p.transition)},
        {"phase",p.phase},{"modorder",std::to_string(p.modOrder)},{"modfamily",p.modFamily},
        {"modcut",number(p.modCutoff)},{"moddith",number(p.modDither)},
        {"oobgain",number(p.oobGain)},{"ahmbits",std::to_string(p.ahmBits)},
        {"sdmfilt",p.sdmFilter},{"sdmfiltnx",p.sdmFilterNx},{"isp",p.isp?"1":"0"},
        {"eq",p.eqFile},{"ir",p.convIr},{"irgpu",p.convGpu?"1":"0"},
        {"roomcorrauto",p.roomCorrAuto?"1":"0"},{"loudness",p.loudness?"1":"0"},
        {"rgmode",p.rgMode},{"rgclip",p.rgClip?"1":"0"},{"cleanup",p.cleanup?"1":"0"},
        {"cleanup_auto",p.cleanupAuto?"1":"0"},{"cleanup_strength",number(p.cleanupStrength)},
        {"cleanup_noise_db",number(p.cleanupNoiseDb)},{"dsdkind",p.dsdKind},{"dsdconv",p.dsdConv},
        {"dsd2stage",p.dsd2Stage?"1":"0"},{"bits",std::to_string(p.bits)},
        {"channels",std::to_string(p.outChannels)},{"trim",trims},{"backend",p.backend},
        {"device",p.device},{"buffer",std::to_string(p.bufferMs)},{"gpuvendor",p.gpuVendor},
        {"upnpdev",p.upnpDev}
    };
}

inline std::string values_json(const Values& values) {
    std::string text = "{";
    for (const auto& [key,value] : values) {
        if (text.size() > 1) text += ',';
        text += '"' + jesc(key) + "\":\"" + jesc(value) + '"';
    }
    return text + '}';
}

inline std::string mode(const PlayParams& p, const rta::dsp::PeqSettings& peq) {
    if (p.directSdm && !raw_dsd_processing(p, &peq).empty()) return "custom";
    bool trim = false;
    for (double value : p.trim) trim |= value != 0.0;
    if ((p.rate.empty() || p.rate == "src") && p.filter.empty() && p.filterNx.empty() &&
        p.shaper == "none" && p.eqFile.empty() && p.convIr.empty() && !p.loudness &&
        !p.isp && !p.cleanup && !p.roomCorrAuto && p.rgMode == "off" && !trim && peq.bypass &&
        (p.dsd == 0 || p.directSdm))
        return "bitperfect";
    if (p.dsd > 0) return "dsd";
    if (p.rate.empty() || p.rate == "src" || p.rate == "follow") return "custom";
    return "pcm";
}

inline std::string style(const PlayParams& p) {
    if (p.taps || p.atten || p.transition || !p.phase.empty() || !p.filterNx.empty() ||
        !p.sdmFilter.empty() || !p.sdmFilterNx.empty()) return "custom";
    if (p.filter == "poly-sinc-gauss-medium") return "warm";
    if (p.filter == "poly-sinc-short-lp") return "balanced";
    if (p.filter == "poly-sinc-long-lp") return "sharp";
    return "custom";
}

struct Snapshot {
    PlayParams params;
    std::string file, revision;
    size_t index = std::string::npos;
    double position = 0.0, duration = 0.0;
    bool playing = false, paused = false, rawVolumeLocked = false;
};

inline Snapshot snapshot_locked() {
    auto& ps = g_ps;
    Snapshot s;
    s.playing = ps.playing.load(); s.paused = ps.paused.load() || ps.pauseReq.load();
    s.params = s.playing && g_paramsInitialized ? g_params : parseForm(form(saved_values()));
    {
        std::lock_guard<std::mutex> lock(ps.mtx);
        s.file = ps.current;
        s.rawVolumeLocked = s.playing && ps.rawVolumeLocked;
        s.params.volume = s.playing ? ps.volDb.load() : saved_volume_setting();
        if (ps.outRate > 0.0) {
            s.position = ps.played.load() / ps.outRate;
            s.duration = ps.total.load() / ps.outRate;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_q.mtx);
        if (g_q.index < g_q.items.size()) {
            s.index = g_q.index;
            const auto& item = g_q.items[s.index];
            if (s.file.empty() || !s.playing) s.file = item.path;
            if (item.path == s.file) { s.params.cueStart = item.cs; s.params.cueEnd = item.ce; }
        }
    }
    s.params.file = s.file;
    if (!g_peq) peq_load_current();
    // Include live PEQ and the queue identity: adjacent CUE tracks can have the
    // same file path, and a stale request must never switch the later track.
    const auto signature = form(parameters(s.params)) + form(saved_values()) + rta::dsp::peq_to_text(g_peq->settings()) +
                           std::to_string(g_workbenchSerial) + ':' + std::to_string(s.index) + ':' +
                           s.file + ':' + number(s.params.cueStart) + ':' + number(s.params.cueEnd);
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : signature) { hash ^= c; hash *= 1099511628211ull; }
    s.revision = std::to_string(hash);
    return s;
}

inline std::string state_locked() {
    const auto s = snapshot_locked();
    auto values = parameters(s.params);
    const auto currentMode = mode(s.params, g_peq->settings()), currentStyle = style(s.params);
    values["ui_profile"] = currentMode; values["ui_style"] = currentStyle;
    std::ostringstream out;
    out << "{\"ok\":true,\"settings\":" << values_json(values)
        << ",\"savedSettings\":" << values_json(parameters(parseForm(form(saved_values()))))
        << ",\"mode\":\"" << currentMode << "\",\"profile\":\"" << currentMode
        << "\",\"style\":\"" << currentStyle << "\",\"playing\":" << (s.playing?"true":"false")
        << ",\"paused\":" << (s.paused?"true":"false") << ",\"file\":\"" << jesc(s.file)
        << "\",\"rawVolumeLocked\":" << (s.rawVolumeLocked?"true":"false")
        << ",\"volumeWritable\":" << (s.rawVolumeLocked?"false":"true")
        << ",\"position\":" << s.position << ",\"duration\":" << s.duration
        << ",\"queueIndex\":";
    if (s.index == std::string::npos) out << "null"; else out << s.index;
    out << ",\"cueStart\":" << s.params.cueStart << ",\"cueEnd\":" << s.params.cueEnd
        << ",\"revision\":\"" << s.revision << "\",\"peq\":" << peq_state_json(true)
        << ",\"volume\":" << s.params.volume << '}';
    return out.str();
}

inline std::string error(int status, const std::string& text) {
    rta::net::webui_set_status(status);
    return "{\"ok\":false,\"error\":\"" + jesc(text) + "\"}";
}

inline bool parse_body(const std::string& body, Values& out, std::string& why) {
    if (body.size() > 65536) { why = "工作台配置过大"; return false; }
    std::istringstream input(body); std::string item;
    while (std::getline(input,item,'&')) {
        const auto split=item.find('=');
        if (split==std::string::npos) { why="配置字段缺少等号"; return false; }
        const auto key=urlDecode(item.substr(0,split)), value=urlDecode(item.substr(split+1));
        if (out.count(key)) { why="配置字段重复："+key; return false; }
        if (!audio_keys().count(key) && key!="expected_file" && key!="expected_revision" &&
            key!="position" && key!="profile" && key!="style" && key!="peq") {
            why="工作台不接受此字段："+key; return false;
        }
        if (!valid_utf8(value)) { why="配置不是有效 UTF-8"; return false; }
        for (unsigned char c:value) if (c<32 || c==127) { why="配置含控制字符"; return false; }
        out[key]=value;
    }
    if (!out.count("expected_file") || !out.count("expected_revision")) {
        why="请先读取当前工作台，再提交配置"; return false;
    }
    return true;
}

inline bool validate_fields(const Values& patch, std::string& why) {
    static const Values enums = {
        {"backend","|wasapi|wasapi-shared|asio|null|upnp|"}, {"rateFamilyPolicy","|prefer|strict|compatible|"},
        {"twostage","|auto|0|1|"}, {"phase","||linear|intermediate|minimum|"}, {"gpuvendor","|auto|amd|nvidia|off|"},
        {"dsdkind","|fir|iir|cic|"}, {"dsdconv","|narrow|wide|xfi|"},
        {"rgmode","|off|track|album|"}, {"modfamily","|dsd|chebyshev|ec|ahm|"},
        {"profile","|bitperfect|pcm|dsd|"}, {"style","|warm|balanced|sharp|"}
    };
    const std::set<std::string> flags={"dsdnative","directsdm","isp","irgpu","roomcorrauto","loudness","rgclip","cleanup","cleanup_auto","dsd2stage"};
    const std::map<std::string,std::pair<double,double>> bounds = {
        {"dsd",{0,1024}},{"bits",{0,32}},{"channels",{0,8}},{"buffer",{10,2000}},
        {"taps",{0,4194304}},{"atten",{0,400}},{"transition",{0,1}},
        {"modorder",{0,15}},{"modcut",{0,1000}},{"moddith",{-1,1}},
        {"oobgain",{0,100}},{"ahmbits",{2,16}},{"cleanup_strength",{0,1}},{"cleanup_noise_db",{-120,-60}}
    };
    for (const auto& [key,value]:patch) {
        if (flags.count(key) && value!="0" && value!="1") {why=key+" 必须为0或1";return false;}
        if (const auto it=enums.find(key);it!=enums.end() && it->second.find('|'+value+'|')==std::string::npos) {why="不支持的 "+key+"："+value;return false;}
        if (const auto it=bounds.find(key);it!=bounds.end()) {
            double n=0;
            if (!parse_num_strict(value,n) || !rta::audio::audio_finite(n) || n<it->second.first || n>it->second.second) {why="参数超出范围："+key;return false;}
            if ((key=="dsd"||key=="bits"||key=="channels"||key=="buffer"||key=="taps"||key=="modorder"||key=="ahmbits") && n!=std::floor(n)) {why="参数须为整数："+key;return false;}
        }
        if ((key=="filter"||key=="filterNx"||key=="sdmfilt"||key=="sdmfiltnx") && !value.empty() && value!="auto" && !rta::dsp::find_preset(value)) {why="未找到滤波器："+value;return false;}
        if (key=="shaper" && value!="none" && std::string("|auto|none|NS1|NS4|NS5|NS9|LNS15|RPDF|TPDF|Gauss1|shaped|").find('|'+value+'|')==std::string::npos) {why="未找到抖动整形器："+value;return false;}
        if (key=="rate" && value!="src" && value!="max" && value!="follow") {
            double n=0;if(!parse_num_strict(value,n)||!rta::audio::audio_finite(n)||n!=std::floor(n)||n<8000||n>49152000){why="输出采样率无效";return false;}
        }
        if (key=="dsd" && value!="0" && value!="64" && value!="128" && value!="256" && value!="512" && value!="1024") {why="DSD倍率无效";return false;}
        if (key=="bits" && value!="0" && value!="16" && value!="24" && value!="32") {why="位深无效";return false;}
        if (key=="trim") {
            std::istringstream input(value);std::string part;size_t count=0;
            while(std::getline(input,part,',')){double n=0;if(!parse_num_strict(part,n)||!rta::audio::audio_finite(n)||n< -24||n>24){why="声道增益无效";return false;}++count;}
            if(count!=8){why="声道增益需要8个值";return false;}
        }
    }
    return true;
}

inline bool parse_peq(const std::string& text, rta::dsp::PeqSettings& settings, std::string& why) {
    rta::net::Json j;
    if (!rta::net::json_parse_strict(text,j,&why) || j.type!=rta::net::Json::Obj) {why="EQ配置JSON无效";return false;}
    auto numeric=[](const rta::net::Json& x,double low,double high){return x.type==rta::net::Json::Num && rta::audio::audio_finite(x.num) && x.num>=low && x.num<=high;};
    auto flag=[](const rta::net::Json& x){return x.type==rta::net::Json::Bool || (x.type==rta::net::Json::Num && (x.num==0||x.num==1));};
    if (!flag(j["bypass"]) || !numeric(j["preamp"],-24,24) || j["bands"].type!=rta::net::Json::Arr || j["bands"].size()>20 || (j.has("name")&&(j["name"].type!=rta::net::Json::Str || j["name"].str.size()>140))) {why="EQ配置的旁路/前级/频段/名称无效";return false;}
    for(unsigned char c:j["name"].str)if(c<32||c==127){why="EQ名称含控制字符";return false;}
    settings.bypass=j["bypass"].type==rta::net::Json::Bool?j["bypass"].b:j["bypass"].num!=0;
    settings.preampDb=j["preamp"].num;
    if(j.has("name"))settings.presetName=j["name"].str;
    settings.bands.clear();
    for(const auto& b:j["bands"].arr){
        rta::dsp::PeqBand band;
        if(!numeric(b["freq"],10,20000)||!numeric(b["gain"],-24,24)||!numeric(b["q"],.1,20)||!flag(b["on"])||!rta::dsp::peq_band_type_from_name(b["type"].s(),band.type)){why="EQ频段参数无效";return false;}
        band.freq=b["freq"].num;band.gainDb=b["gain"].num;band.q=b["q"].num;
        band.enabled=b["on"].type==rta::net::Json::Bool?b["on"].b:b["on"].num!=0;
        settings.bands.push_back(band);
    }
    return true;
}

inline bool plan_profile(const Values& patch, PlayParams& candidate, rta::dsp::PeqSettings& peq, std::string& why) {
    if (!patch.count("profile")) return true;
    using namespace rta::engine;
    SourceInfo source;
    if (!candidate.file.empty()) {
        const auto info=rta::audio::probe_media(candidate.file);
        source.srcRate=info.sampleRate>0?(uint32_t)info.sampleRate:44100;
        source.srcBits=info.bits>0?info.bits:16;
        const auto ext=rta::audio::lowerExt(candidate.file);
        source.isDsd=ext==".dsf"||ext==".dff";
        if(source.isDsd){const auto dsd=probe_dsd(candidate.file);source.srcRate=dsd.rate;source.srcBits=1;}
    }
    const auto name=patch.at("profile");
    PresetProfile profile=name=="bitperfect"?PresetProfile::BitPerfect:name=="pcm"?PresetProfile::Pcm:source.isDsd?PresetProfile::Dsd:PresetProfile::PcmToDsd;
    auto caps=preset_probe_caps(&candidate);
    if(profile!=PresetProfile::BitPerfect){
        // This path retains software gain and processing, so its deliverable
        // transport is DoP. Native-only capacity cannot promise a DoP rate.
        caps.dsdNative=false;caps.dsdNativeMax.clear();
        const uint32_t base=source.srcRate%48000u==0?48000u:44100u;
        int level=0;
        for(int candidateLevel:{64,128,256,512,1024}){
            const uint32_t carrier=base*(uint32_t)candidateLevel/16u;
            if(std::find(caps.rates.begin(),caps.rates.end(),carrier)!=caps.rates.end())level=candidateLevel;
        }
        caps.dop=level>0;caps.dopMax=level?rta::engine::dsd_level_name(level):"";
    }
    if(profile!=PresetProfile::BitPerfect && caps.rates.empty()){why="无法确认当前设备的升频能力";return false;}
    if(profile==PresetProfile::Dsd||profile==PresetProfile::PcmToDsd){if(!caps.dop&&!caps.dsdNative){why="当前输出不支持DSD";return false;}}
    const auto plan=plan_preset(profile,caps,source);
    candidate.rate=plan.rate;candidate.filter=plan.filter;candidate.filterNx.clear();
    candidate.sdmFilter.clear();candidate.sdmFilterNx.clear();candidate.shaper=plan.shaper;
    candidate.taps=0;candidate.atten=0;candidate.transition=0;candidate.phase.clear();
    candidate.dsd=plan.dsd;candidate.directSdm=plan.directSdm;candidate.twoStage=-1;
    if(!plan.modFamily.empty())candidate.modFamily=plan.modFamily;
    if(plan.modOrder>0)candidate.modOrder=plan.modOrder;
    // PCM sources and processed DSD use DoP. A raw Native path requires zero
    // gain and cannot honor the retained software volume of an A/B session.
    if(profile!=PresetProfile::BitPerfect)candidate.dsdNative=false;
    if(profile==PresetProfile::BitPerfect){
        candidate.eqFile.clear();candidate.convIr.clear();candidate.convGpu=false;
        candidate.loudness=false;candidate.isp=false;candidate.roomCorrAuto=false;
        candidate.cleanup=false;candidate.rgMode="off";peq.bypass=true;
        for(double& trim:candidate.trim)trim=0;
    }
    return true;
}

inline std::string apply(const std::string& body, bool sessionAlreadyLocked = false, bool syncProfile = true) {
    Values patch;std::string why;
    if(!parse_body(body,patch,why)||!validate_fields(patch,why))return error(400,why);
    std::unique_lock<std::mutex> session(g_ps.sessionMx,std::defer_lock);
    if(!sessionAlreadyLocked)session.lock();
    const auto before=snapshot_locked();
    if(patch.at("expected_file")!=before.file||patch.at("expected_revision")!=before.revision)
        return error(409,"播放曲目或参数已变化，请刷新工作台后再切换");
    if(g_upnpPlayer.load()||g_cdActive.load()||g_inputSession.running.load())
        return error(409,"当前是网络、光盘或输入监听会话，请先停止后再切换");
    if(patch.size()<=2)return state_locked();
    PlayParams candidate=before.params;
    auto values=parameters(candidate);
    for(const auto& [key,value]:patch)if(audio_keys().count(key))values[key]=value;
    candidate=parseForm(form(values));candidate.file=before.file;
    candidate.cueStart=before.params.cueStart;candidate.cueEnd=before.params.cueEnd;
    candidate.volume=before.params.volume;
    const auto previousPeq=g_peq->settings();
    auto peq=previousPeq;
    if(patch.count("peq")&&!parse_peq(patch.at("peq"),peq,why))return error(400,why);
    if(!plan_profile(patch,candidate,peq,why))return error(400,why);
    if(patch.count("style")){
        if(mode(candidate,peq)=="bitperfect")return error(400,"原音直通不应用听音风格，请先选择升频方式");
        const auto value=patch.at("style");
        candidate.filter=value=="warm"?"poly-sinc-gauss-medium":value=="sharp"?"poly-sinc-long-lp":"poly-sinc-short-lp";
        candidate.filterNx.clear();candidate.sdmFilter.clear();candidate.sdmFilterNx.clear();
        candidate.taps=0;candidate.atten=0;candidate.transition=0;candidate.phase.clear();
        candidate.directSdm=false;candidate.dsdNative=false;
    }
    double position=before.position;
    if(patch.count("position")&&(!parse_num_strict(patch.at("position"),position)||!rta::audio::audio_finite(position)||position<0||!before.playing||before.duration<=0||position>=before.duration))return error(400,"对比起点必须在当前曲目区间内");
    if(candidate.backend=="upnp" && before.playing)return error(409,"网络输出请停止播放后选择，无法保证同位置切换");
    if(candidate.dsd>0 && candidate.backend=="wasapi-shared")return error(400,"DSD输出需要独占或设备直连");
    if(candidate.dsdNative && !candidate.file.empty() && !native_dsd_precheck(candidate,why,&peq))return error(400,why);
    if(!candidate.eqFile.empty()&&!fs::is_regular_file(fs::u8path(candidate.eqFile)))return error(400,"EQ文件不存在");
    if(!candidate.convIr.empty()&&!fs::is_regular_file(fs::u8path(candidate.convIr)))return error(400,"卷积文件不存在");
    // Query the selected endpoint before stopping its old session. Null is a
    // software-only capability model and never touches a default audio device.
    if(candidate.backend!="null"&&candidate.backend!="upnp"){
        const auto caps=preset_probe_caps(&candidate);
        if(candidate.backend!="wasapi-shared"&&caps.rates.empty())return error(400,"无法确认所选设备可用，原会话继续播放");
        if(candidate.dsd>0&&!candidate.dsdNative){
            const auto ext=rta::audio::lowerExt(candidate.file);
            const bool dsdSource=ext==".dsf"||ext==".dff";
            const auto pcmInfo=dsdSource?rta::audio::MediaInfo{}:rta::audio::probe_media(candidate.file);
            const auto sourceRate=dsdSource?probe_dsd(candidate.file).rate:
                pcmInfo.sampleRate>0?(uint32_t)pcmInfo.sampleRate:44100u;
            const auto base=sourceRate%48000u==0?48000u:44100u;
            const uint32_t carrier=base*(uint32_t)candidate.dsd/16u;
            if(std::find(caps.rates.begin(),caps.rates.end(),carrier)==caps.rates.end())return error(400,"所选设备不支持此DSD倍率的DoP载波");
        }else if(candidate.rate!="src"&&candidate.rate!="follow"&&candidate.rate!="max"&&!candidate.rate.empty()){
            const auto rate=(uint32_t)std::strtoul(candidate.rate.c_str(),nullptr,10);
            if(std::find(caps.rates.begin(),caps.rates.end(),rate)==caps.rates.end())return error(400,"所选设备不支持此采样率");
        }
    }
    // A track may naturally finish while capability queries run.
    const auto checked=snapshot_locked();
    if(checked.file!=before.file||checked.revision!=before.revision||checked.playing!=before.playing)
        return error(409,"当前曲目在准备期间已变化，请重新选择对比配置");
    if(before.playing && before.duration<=0)return error(409,"播放尚未就绪，请等待信号链建立后再切换");
    if(before.playing){
        // All queue-edit endpoints take sessionMx before the queue lock. The
        // old owner can advance its index during join, but cannot remove or
        // replace this validated entry once persistence begins.
        std::lock_guard<std::mutex> lock(g_q.mtx);
        const QItem wanted(before.file,before.params.cueStart,before.params.cueEnd);
        if(before.index>=g_q.items.size()||!g_q.items[before.index].same(wanted))
            return error(409,"队列项已变化，配置未切换");
    }
    const auto previousSettings=saved_values();auto merged=previousSettings;
    const auto updated=parameters(candidate);
    for(const auto& [key,value]:updated)merged[key]=value;
    // Live session gain is authoritative even when it differs from the last
    // saved slider value. A/B cannot reset or replace it.
    merged["volume"]=number(candidate.volume);
    auto bytes=[](const Values& map){std::string out;for(const auto& [k,v]:map)out+=k+'='+v+'\n';return out;};
    const auto settingsPath=fs::u8path(path_in_exe_dir("rlplayer-settings.conf"));
    if(!rta::meta::meta_write_atomic(settingsPath,bytes(merged)))return error(500,"配置保存失败，原播放参数保留");
    if(!rta::meta::meta_write_atomic(fs::u8path(peq_current_path()),rta::dsp::peq_to_text(peq))){
        const bool restored=rta::meta::meta_write_atomic(settingsPath,bytes(previousSettings));
        return error(500,restored?"EQ保存失败，原配置已恢复":"EQ保存失败，设置文件回滚也失败，请检查目录权限");
    }
    if(before.playing)g_ps.session_stop_join_locked();
    g_peq->set_settings(peq);
    establish_session_params_locked(candidate);
    g_ps.seekReq.store(-1.0);g_ps.pauseReq.store(before.playing&&before.paused);g_ps.paused.store(false);
    {
        std::lock_guard<std::mutex> lock(g_ps.mtx);
        g_ps.error.clear();g_ps.signal.clear();
    }
    if(before.playing){
        size_t index=before.index;
        {std::lock_guard<std::mutex> lock(g_q.mtx);
            g_q.index=index;
        }
        candidate.workbenchResume=position;candidate.workbenchPaused=before.paused;candidate.workbenchStrict=true;
        candidate.queueIndex=index;
        g_ps.workbenchReady.store(false);
        g_ps.played.store(0);g_ps.total.store(0);g_ps.playing.store(true);
        g_ps.th=std::thread(queue_thread,candidate,index);
        play_log("WORKBENCH_APPLY position="+number(position)+" paused="+(before.paused?"1":"0")+" file="+before.file);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(24);
        while(g_ps.playing.load() &&
              (!g_ps.workbenchReady.load() || (before.paused&&!g_ps.paused.load())) &&
              std::chrono::steady_clock::now()<deadline) Sleep(10);
        if(!g_ps.playing.load() || !g_ps.workbenchReady.load() || (before.paused&&!g_ps.paused.load())) {
            std::string failure;
            {std::lock_guard<std::mutex> lock(g_ps.mtx);failure=g_ps.error;}
            g_ps.session_stop_join_locked();
            const bool settingsRestored=rta::meta::meta_write_atomic(settingsPath,bytes(previousSettings));
            const bool peqRestored=rta::meta::meta_write_atomic(fs::u8path(peq_current_path()),rta::dsp::peq_to_text(previousPeq));
            g_peq->set_settings(previousPeq);
            PlayParams restore=before.params;
            restore.workbenchResume=before.position;restore.workbenchPaused=before.paused;restore.workbenchStrict=true;
            establish_session_params_locked(restore);
            g_ps.pauseReq.store(before.paused);g_ps.seekReq.store(-1);g_ps.workbenchReady.store(false);
            g_ps.playing.store(true);
            g_ps.th=std::thread(queue_thread,restore,index);
            play_log("WORKBENCH_ROLLBACK file="+before.file+" reason="+failure);
            return error(500,"切换失败，正在恢复原播放配置："+(failure.empty()?std::string("准备超时"):failure)+
                ((!settingsRestored||!peqRestored)?"；设置文件回滚失败，请检查目录权限":""));
        }
    }
    if(syncProfile&&!sync_active_locked())return error(500,"参数已应用，但当前配置组保存失败，请检查目录权限");
    return state_locked();
}

} // namespace workbench

#include "workbench_profiles.h"
