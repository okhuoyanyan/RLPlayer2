#pragma once

// Included by workbench_config.h after its validated transactional apply path.
// Every access holds g_ps.sessionMx. Profile storage contains only processing
// parameters; endpoint, gain, track and playback position never enter this file.
namespace workbench {

inline bool profile_key(const std::string& k) {
    return audio_keys().count(k)&&k!="backend"&&k!="device"&&k!="buffer"&&k!="upnpdev"&&k!="gpuvendor";
}
inline Values profile_values(const PlayParams& p) {
    auto v=parameters(p);
    for(auto it=v.begin();it!=v.end();)if(!profile_key(it->first))it=v.erase(it);else ++it;
    return v;
}
inline std::string peq_json(const rta::dsp::PeqSettings& p) {
    std::string s="{\"bypass\":"+std::string(p.bypass?"true":"false")+",\"preamp\":"+number(p.preampDb)+
        ",\"name\":\""+jesc(p.presetName)+"\",\"bands\":[";
    for(size_t i=0;i<p.bands.size();++i){
        const auto& b=p.bands[i];if(i)s+=',';
        s+="{\"i\":"+std::to_string(i+1)+",\"on\":"+(b.enabled?"true":"false")+",\"type\":\""+
            rta::dsp::peq_band_type_name(b.type)+"\",\"freq\":"+number(b.freq)+",\"gain\":"+number(b.gainDb)+",\"q\":"+number(b.q)+'}';
    }
    return s+"]}";
}
struct ProfileSlot {
    std::string id,name,eqId,eqName;
    Values settings;
    rta::dsp::PeqSettings peq;
    bool migrated=false;
};
struct EqSlot {
    std::string id,name,source="custom",productId,presetId,link;
    rta::dsp::PeqSettings peq;
    bool exists=false;
};
struct ProfileStore {
    bool loaded=false,pendingSaved=false;std::string loadError;int active=0;
    std::vector<ProfileSlot> slots;
    std::vector<EqSlot> eqs;
};
inline ProfileStore g_profiles;
inline std::string profile_path(){return path_in_exe_dir("rlplayer-workbench-profiles.json");}
inline int eq_id(const std::string& text){for(int i=1;i<=16;++i)if(text==std::to_string(i))return i;return 0;}

inline std::string profile_slots_json() {
    std::string s="[";
    for(const auto& p:g_profiles.slots){if(s.size()>1)s+=',';
        s+="{\"id\":\""+p.id+"\",\"name\":\""+jesc(p.name)+"\",\"settings\":"+values_json(p.settings)+
            ",\"peq\":"+peq_json(p.peq)+",\"eqId\":\""+p.eqId+"\",\"eqName\":\""+jesc(p.eqName)+
            "\",\"migrated\":"+(p.migrated?"true":"false")+'}';
    }
    return s+']';
}
inline std::string eq_slots_json() {
    std::string s="[";
    for(const auto& p:g_profiles.eqs){if(s.size()>1)s+=',';
        s+="{\"id\":\""+p.id+"\",\"name\":\""+jesc(p.name)+"\",\"source\":\""+p.source+
            "\",\"productId\":\""+jesc(p.productId)+"\",\"presetId\":\""+jesc(p.presetId)+"\",\"link\":\""+jesc(p.link)+
            "\",\"empty\":"+(p.exists?"false":"true")+",\"peq\":"+(p.exists?peq_json(p.peq):"null")+'}';
    }
    return s+']';
}
inline bool persist_profiles_locked() {
    return rta::meta::meta_write_atomic(fs::u8path(profile_path()),"{\"version\":1,\"active\":\""+
        std::string(1,char('A'+g_profiles.active))+"\",\"slots\":"+profile_slots_json()+",\"eqs\":"+eq_slots_json()+"}\n");
}
inline std::string json_text(const rta::net::Json& j) {
    // PEQ fields are the only object payload in this persistence format.
    if(j.type==rta::net::Json::Str)return '"'+jesc(j.str)+'"';
    if(j.type==rta::net::Json::Num)return number(j.num);
    if(j.type==rta::net::Json::Bool)return j.b?"true":"false";
    std::string s=j.type==rta::net::Json::Arr?"[":"{";
    if(j.type==rta::net::Json::Arr){for(const auto& x:j.arr){if(s.size()>1)s+=',';s+=json_text(x);}return s+']';}
    for(const auto& [k,v]:j.obj){if(s.size()>1)s+=',';s+='"'+jesc(k)+"\":"+json_text(v);}return s+'}';
}
inline bool valid_name(const std::string& s) {
    if(s.empty()||s.size()>140||!valid_utf8(s))return false;
    for(unsigned char c:s)if(c<32||c==127)return false;
    return s.find_first_not_of(' ')!=std::string::npos;
}
inline bool ensure_profiles_locked() {
    if(g_profiles.loaded)return g_profiles.loadError.empty();
    g_profiles.loaded=true;
    if(!g_peq)peq_load_current();
    const auto initial=profile_values(parseForm(form(saved_values())));
    for(int i=0;i<4;++i){ProfileSlot p;p.id=std::string(1,char('A'+i));p.name=p.id;p.settings=initial;p.peq=g_peq->settings();g_profiles.slots.push_back(p);}
    for(int i=1;i<=16;++i){EqSlot p;p.id=std::to_string(i);g_profiles.eqs.push_back(p);}
    std::ifstream in(fs::u8path(profile_path()),std::ios::binary);
    if(!in){if(persist_profiles_locked())return true;g_profiles.loadError="无法初始化配置组文件，请检查目录权限";return false;}
    const std::string bytes((std::istreambuf_iterator<char>(in)),{});
    rta::net::Json j;std::string why;
    auto bad=[&](){g_profiles.loadError="配置组文件损坏，保留原文件："+why;return false;};
    if(bytes.size()>1024*1024||!rta::net::json_parse_strict(bytes,j,&why)||j["version"].n()!=1||j["slots"].size()!=4)return bad();
    for(int i=0;i<4;++i){const auto& x=j["slots"].at(i);auto& p=g_profiles.slots[i];
        if(x["id"].s()!=p.id||!valid_name(x["name"].s())||x["settings"].type!=rta::net::Json::Obj){why="配置组身份/名称无效";return bad();}
        Values patch;for(const auto& [k,v]:x["settings"].obj){if(!profile_key(k)||v.type!=rta::net::Json::Str){why="配置组字段不合法";return bad();}patch[k]=v.str;}
        if(!validate_fields(patch,why)||!parse_peq(json_text(x["peq"]),p.peq,why))return bad();
        for(const auto& [k,v]:patch)p.settings[k]=v;
        p.name=x["name"].str;p.eqId=x["eqId"].s();p.eqName=x["eqName"].s();p.migrated=x["migrated"].b;
        if(!p.eqId.empty()&&!eq_id(p.eqId)){why="均衡方案身份无效";return bad();}
    }
    const auto active=j["active"].s();if(active.size()!=1||active[0]<'A'||active[0]>'D'){why="当前配置组无效";return bad();}g_profiles.active=active[0]-'A';
    if(j["eqs"].type==rta::net::Json::Arr)for(const auto& x:j["eqs"].arr){
        const int id=eq_id(x["id"].s());if(!id||x["empty"].b||x["peq"].type==rta::net::Json::Null)continue;
        auto& p=g_profiles.eqs[id-1];
        if(!valid_name(x["name"].s())||!parse_peq(json_text(x["peq"]),p.peq,why))return bad();
        p.exists=true;p.name=x["name"].s();p.source=x["source"].s()=="opra"?"opra":"custom";
        p.productId=x["productId"].s();p.presetId=x["presetId"].s();p.link=x["link"].s();
    }
    const auto& activeSlot=g_profiles.slots[g_profiles.active];
    g_profiles.pendingSaved=activeSlot.settings!=profile_values(parseForm(form(saved_values())))||peq_json(activeSlot.peq)!=peq_json(g_peq->settings());
    return true;
}
inline bool sync_active_locked() {
    if(!ensure_profiles_locked())return false;
    auto& p=g_profiles.slots[g_profiles.active];p.settings=profile_values(snapshot_locked().params);p.peq=g_peq->settings();p.migrated=true;
    g_profiles.pendingSaved=false;
    if(!p.eqId.empty()){
        const int id=eq_id(p.eqId);
        if(id<1||id>16||!g_profiles.eqs[id-1].exists||peq_json(g_profiles.eqs[id-1].peq)!=peq_json(p.peq)){p.eqId.clear();p.eqName.clear();}
    }
    return persist_profiles_locked();
}
inline bool sync_saved_locked() {
    if(!ensure_profiles_locked())return false;
    auto& p=g_profiles.slots[g_profiles.active];p.settings=profile_values(parseForm(form(saved_values())));p.migrated=true;
    const auto now=snapshot_locked();g_profiles.pendingSaved=now.playing&&p.settings!=profile_values(now.params);
    return persist_profiles_locked();
}
inline bool sync_peq_locked() {
    if(!ensure_profiles_locked())return false;
    auto& p=g_profiles.slots[g_profiles.active];p.peq=g_peq->settings();p.eqId.clear();p.eqName.clear();p.migrated=true;
    return persist_profiles_locked();
}
inline bool request_values(const std::string& body,Values& out,std::string& why) {
    if(body.size()>65536){why="请求过大";return false;}
    std::istringstream in(body);std::string token;
    while(std::getline(in,token,'&')){const auto at=token.find('=');if(at==std::string::npos){why="字段缺少等号";return false;}
        const auto k=urlDecode(token.substr(0,at)),v=urlDecode(token.substr(at+1));
        if(out.count(k)||!valid_utf8(k)||!valid_utf8(v)){why="重复字段或UTF-8无效";return false;}
        for(unsigned char c:v)if(c<32||c==127){why="字段含控制字符";return false;}out[k]=v;
    }return true;
}
inline bool result_ok(const std::string& s){rta::net::Json j;return rta::net::json_parse_strict(s,j)&&j["ok"].b;}
inline bool restore_audio_locked(const Snapshot& before,const rta::dsp::PeqSettings& peq) {
    const auto current=snapshot_locked();auto patch=parameters(before.params);
    patch["peq"]=peq_json(peq);patch["expected_file"]=current.file;patch["expected_revision"]=current.revision;
    if(before.playing&&current.playing&&before.duration>0&&before.position<before.duration)patch["position"]=number(before.position);
    return result_ok(apply(form(patch),true,false));
}
inline std::string loop_json_locked() {
    auto p=g_segment.get();const auto current=snapshot_locked();
    if(p.enabled&&!p.matches(current.file,current.params.cueStart,current.params.cueEnd,current.index)){p.enabled=false;g_segment.set(p);}
    return "{\"enabled\":"+std::string(p.enabled?"true":"false")+",\"repeat\":"+(p.repeat?"true":"false")+
        ",\"start\":"+number(p.start)+",\"end\":"+number(p.end)+",\"file\":\""+jesc(p.file)+"\",\"cueStart\":"+number(p.cueStart)+",\"cueEnd\":"+number(p.cueEnd)+'}';
}
inline std::string loop_request(const std::string& body) {
    Values f;std::string why;if(!request_values(body,f,why))return error(400,why);
    std::unique_lock<std::mutex> session(g_ps.sessionMx);
    if(f["enabled"]!="0"&&f["enabled"]!="1")return error(400,"enabled必须为0或1");
    const auto current=snapshot_locked();
    if(f.count("expected_revision")&&f["expected_revision"]!=current.revision)return error(409,"当前曲目或配置已改变，请刷新片段");
    if(f.count("file")&&f["file"]!=current.file)return error(409,"当前曲目已改变，请刷新片段");
    auto s=g_segment.get();
    if(f["enabled"]=="0"){s.enabled=false;g_segment.set(s);return "{\"ok\":true,\"loop\":"+loop_json_locked()+'}';}
    // 2026-11-12：允许暂停中设置片段（A/B 对比本来就要先定段再听）。
    // duration 仍必须已知；停止/未开流时没有 duration，不能凭空写一个越界区间。
    if((!current.playing&&!current.paused)||current.duration<=0||g_upnpPlayer.load()||g_cdActive.load()||g_inputSession.running.load())return error(409,"请先播放（或暂停）一首本地曲目，再设置片段");
    if(f["file"]!=current.file)return error(409,"当前曲目已改变，请刷新片段");
    if(!parse_num_strict(f["start"],s.start)||!parse_num_strict(f["end"],s.end)||!rta::audio::audio_finite(s.start)||!rta::audio::audio_finite(s.end)||s.start<0||s.start>=s.end||s.end>current.duration)return error(400,"片段须满足0≤起点<终点≤当前曲目时长（CUE使用本轨相对秒数）");
    if(f.count("repeat")&&f["repeat"]!="0"&&f["repeat"]!="1")return error(400,"repeat必须为0或1");
    s.enabled=true;s.repeat=!f.count("repeat")||f["repeat"]=="1";s.file=current.file;s.cueStart=current.params.cueStart;s.cueEnd=current.params.cueEnd;s.queueIndex=current.index;
    g_segment.set(s);
    if(current.position<s.start||current.position>=s.end)g_ps.seekReq.store(s.start/current.duration);
    return "{\"ok\":true,\"loop\":"+loop_json_locked()+'}';
}
inline std::string profiles_json_locked() {
    if(!ensure_profiles_locked())return error(500,g_profiles.loadError);
    return "{\"ok\":true,\"active\":\""+std::string(1,char('A'+g_profiles.active))+"\",\"slots\":"+profile_slots_json()+",\"loop\":"+loop_json_locked()+'}';
}
inline std::string eqpresets_json_locked() {
    if(!ensure_profiles_locked())return error(500,g_profiles.loadError);
    return "{\"ok\":true,\"slots\":"+eq_slots_json()+",\"activeId\":\""+g_profiles.slots[g_profiles.active].eqId+"\",\"peq\":"+peq_json(g_peq->settings())+'}';
}
inline std::string profile_request(const std::string& body) {
    Values f;std::string why;if(!request_values(body,f,why))return error(400,why);
    std::unique_lock<std::mutex> session(g_ps.sessionMx);
    if(!ensure_profiles_locked())return error(500,g_profiles.loadError);
    if(f["id"].size()!=1||f["id"][0]<'A'||f["id"][0]>'D')return error(400,"配置组必须为A到D");
    const int id=f["id"][0]-'A';const auto action=f["action"];
    if(action!="select"&&action!="update"&&action!="rename"&&action!="migrate")return error(400,"未知配置组操作");
    if(action=="update"&&id!=g_profiles.active)return error(409,"当前配置组已改变，请刷新后再编辑");
    for(const auto& [k,v]:f)if(!profile_key(k)&&k!="action"&&k!="id"&&k!="name"&&k!="peq"&&k!="eqId"&&k!="profile"&&k!="style"&&k!="position")return error(400,"配置组不接受此字段："+k);
    const auto previous=g_profiles;const auto beforeAudio=snapshot_locked();const auto beforePeq=g_peq->settings();bool applied=false;
    if(action=="rename"){
        if(!valid_name(f["name"]))return error(400,"配置组名称不能为空或超过140字节");
        g_profiles.slots[id].name=f["name"];g_profiles.slots[id].migrated=true;
    }else if(action=="migrate"){
        auto& p=g_profiles.slots[id];if(p.migrated)return profiles_json_locked();
        Values patch;
        for(const auto& [k,v]:f)if(profile_key(k))patch[k]=v;
        if(!validate_fields(patch,why))return error(400,why);
        if(f.count("peq")&&!parse_peq(f["peq"],p.peq,why))return error(400,why);
        for(const auto& [k,v]:patch)p.settings[k]=v;
        if(f.count("name")&&valid_name(f["name"]))p.name=f["name"];
        p.migrated=true;
        if(id==g_profiles.active)g_profiles.pendingSaved=true;
    }else{
        if(action=="select"){
            auto& current=g_profiles.slots[g_profiles.active];const auto live=profile_values(beforeAudio.params);
            // Settings can describe the next processing state while the old
            // renderer is still active. They are already saved, so do not
            // replace that accepted edit with an obsolete live snapshot.
            if(!g_profiles.pendingSaved||current.settings==live)current.settings=live;
            if(!g_profiles.pendingSaved)current.peq=g_peq->settings();
        }
        auto& target=g_profiles.slots[id];Values patch;
        if(action=="select")patch=target.settings;
        else for(const auto& [k,v]:f)if(profile_key(k)||k=="peq"||k=="profile"||k=="style"||k=="position")patch[k]=v;
        if(action=="select")patch["peq"]=peq_json(target.peq);
        if(f.count("eqId")){
            const int e=eq_id(f["eqId"]);
            if(e<1||e>16||!g_profiles.eqs[e-1].exists)return error(400,"请选择已保存的自定义或OPRA均衡");
            const auto& eq=g_profiles.eqs[e-1];patch["peq"]=peq_json(eq.peq);patch["eq"]="";
            target.eqId=eq.id;target.eqName=eq.name;
        }
        const auto snap=snapshot_locked();patch["expected_file"]=snap.file;patch["expected_revision"]=snap.revision;
        const auto result=apply(form(patch),true,false);
        if(!result_ok(result)){g_profiles=previous;return result;}
        applied=true;
        g_profiles.pendingSaved=false;
        g_profiles.active=id;target.settings=profile_values(snapshot_locked().params);target.peq=g_peq->settings();target.migrated=true;
        if(action=="update"&&f.count("peq")&&!f.count("eqId")){target.eqId.clear();target.eqName.clear();}
    }
    if(!persist_profiles_locked()){g_profiles=previous;const bool restored=!applied||restore_audio_locked(beforeAudio,beforePeq);return error(500,restored?"配置组保存失败，已保留切换前配置":"配置组保存失败，播放配置回滚也失败，请停止后重新选择");}
    return profiles_json_locked();
}

inline std::string eqpresets_request(const std::string& body) {
    Values f;std::string why;if(!request_values(body,f,why))return error(400,why);
    std::unique_lock<std::mutex> session(g_ps.sessionMx);
    if(!ensure_profiles_locked())return error(500,g_profiles.loadError);
    const auto action=f["action"];
    if(f.count("slotId")&&f["slotId"]!=std::string(1,char('A'+g_profiles.active)))return error(409,"当前配置组已改变，请刷新后重试");
    int id=eq_id(f["id"]);
    if(action=="import"&&f["id"].empty())for(int i=0;i<16;++i)if(!g_profiles.eqs[i].exists){id=i+1;break;}
    if(id<1||id>16)return error(400,"均衡位置必须为1到16；请先选择一个可用位置");
    const auto previous=g_profiles;const auto beforeAudio=snapshot_locked();const auto beforePeq=g_peq->settings();bool applied=false;
    auto& eq=g_profiles.eqs[id-1];auto& active=g_profiles.slots[g_profiles.active];
    if(action=="delete"){
        eq=EqSlot{};eq.id=std::to_string(id);
        for(auto& p:g_profiles.slots)if(p.eqId==eq.id){p.eqId.clear();p.eqName.clear();}
    }else if(action=="rename"){
        if(!eq.exists)return error(400,"这个均衡位置尚未保存");
        if(!valid_name(f["name"]))return error(400,"请填写均衡方案名称");
        eq.name=f["name"];
        for(auto& p:g_profiles.slots)if(p.eqId==eq.id)p.eqName=eq.name;
        if(!persist_profiles_locked())return error(500,"均衡改名保存失败");
        return eqpresets_json_locked();
    }else if(action=="save"||action=="import"||action=="load"){
        auto peq=g_peq->settings();
        if(action=="load"){
            if(!eq.exists)return error(400,"这个均衡位置尚未保存");peq=eq.peq;
        }else if(action=="save"){
            if(f.count("peq")&&!parse_peq(f["peq"],peq,why))return error(400,why);
            if(!valid_name(f["name"]))return error(400,"请填写均衡方案名称");
            eq.name=f["name"];eq.source="custom";eq.productId.clear();eq.presetId.clear();eq.link.clear();
        }else{
            std::vector<rta::dsp::OpraPreset> presets;bool cached=false;
            auto& service=rta::dsp::OpraService::instance();service.init(path_in_exe_dir("data\\opra_index.json"),path_in_exe_dir(""));
            if(f["productId"].empty()||f["presetId"].empty())return error(400,"缺少OPRA产品或方案");
            if(!service.get_presets(f["productId"],presets,cached,why))return error(400,why);
            const auto found=std::find_if(presets.begin(),presets.end(),[&](const auto& p){return p.id==f["presetId"];});
            if(found==presets.end())return error(400,"未找到指定OPRA方案");
            if(found->bands.size()>20||found->gainDb< -24||found->gainDb>24)return error(400,"该OPRA方案超过20段或前级范围，不能无损导入当前均衡");
            peq.bypass=false;peq.preampDb=found->gainDb;peq.bands.clear();
            for(const auto& b:found->bands){rta::dsp::PeqBand band;
                if(!rta::dsp::peq_band_type_from_name(rta::dsp::opra_type_to_eq_type(b.type),band.type))return error(400,"该OPRA方案含当前PEQ不支持的滤波类型："+b.type);
                band.freq=b.frequency;band.gainDb=b.gainDb;band.q=b.q;band.enabled=true;peq.bands.push_back(band);
            }
            eq.source="opra";eq.productId=f["productId"];eq.presetId=f["presetId"];eq.link=found->link;
            eq.name=valid_name(f["name"])?f["name"]:found->author+(found->details.empty()?"":" · "+found->details);
            if(!valid_name(eq.name))eq.name="OPRA "+std::to_string(id);
        }
        peq.presetName=eq.name;
        if(!parse_peq(peq_json(peq),peq,why)){g_profiles=previous;return error(400,why);}
        const auto snap=snapshot_locked();Values patch={{"expected_file",snap.file},{"expected_revision",snap.revision},{"peq",peq_json(peq)},{"eq",""}};
        const auto result=apply(form(patch),true,false);
        if(!result_ok(result)){g_profiles=previous;return result;}
        applied=true;
        eq.exists=true;eq.peq=peq;active.peq=peq;active.eqId=eq.id;active.eqName=eq.name;active.settings=profile_values(snapshot_locked().params);active.migrated=true;
    }else return error(400,"未知均衡操作");
    if(!persist_profiles_locked()){g_profiles=previous;const bool restored=!applied||restore_audio_locked(beforeAudio,beforePeq);return error(500,restored?"均衡方案保存失败，已保留原配置":"均衡方案保存失败，播放配置回滚也失败，请停止后重新选择");}
    return eqpresets_json_locked();
}

} // namespace workbench
