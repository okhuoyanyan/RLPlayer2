// Windows launcher-side transactional install, protected user data and rollback.
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "net/update.h"
#include "net/update_assets_hashes.h"

namespace rta::update {

inline std::string pending_path(){return join_path(updates_root(),"pending.txt");}
struct PendingPlan {
    bool exists=false;std::string phase,version,from,staging,time,detail;
    std::vector<std::string> files,applied,added,deletes,deleted;
    std::string operation;bool operationExisted=false;
};
inline PendingPlan read_pending(){
    PendingPlan p;if(!path_exists(pending_path()))return p;p.exists=true;const std::string text=read_text_file(pending_path());size_t pos=0;
    while(pos<text.size()){size_t end=text.find('\n',pos);if(end==std::string::npos)end=text.size();std::string line=text.substr(pos,end-pos);pos=end+1;
        while(!line.empty()&&(line.back()=='\r'||line.back()==' '))line.pop_back();const size_t equal=line.find('=');if(equal==std::string::npos)continue;
        const std::string key=line.substr(0,equal),value=line.substr(equal+1);
        if(key=="phase")p.phase=value;else if(key=="version")p.version=value;else if(key=="from")p.from=value;
        else if(key=="staging")p.staging=value;else if(key=="time")p.time=value;else if(key=="detail")p.detail=value;
        else if(key=="file")p.files.push_back(value);else if(key=="applied")p.applied.push_back(value);
        else if(key=="added")p.added.push_back(value);else if(key=="delete")p.deletes.push_back(value);
        else if(key=="deleted")p.deleted.push_back(value);else if(key=="operation")p.operation=value;
        else if(key=="operationExisted")p.operationExisted=value=="1";}
    return p;
}
inline std::string update_temporary_path(const std::string& target){
    static std::atomic<unsigned long long> serial{0};return target+".update-tmp-"+std::to_string(GetCurrentProcessId())+"-"+
        std::to_string(GetTickCount64())+"-"+std::to_string(serial.fetch_add(1));
}
inline bool write_recovery_ledger_atomic(const std::string& target,const std::string& text){
    const auto temp=rta::platform::from_utf8(update_temporary_path(target));
    HANDLE handle=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;bool ok=true;
    for(size_t offset=0;ok&&offset<text.size();){const DWORD count=DWORD(std::min<size_t>(text.size()-offset,1u<<20));DWORD written=0;
        ok=WriteFile(handle,text.data()+offset,count,&written,nullptr)&&written==count;offset+=written;}
    if(ok)ok=FlushFileBuffers(handle)!=0;CloseHandle(handle);
    if(ok)ok=MoveFileExW(temp.c_str(),rta::platform::from_utf8(target).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok)DeleteFileW(temp.c_str());return ok;
}
inline bool write_pending(const PendingPlan& p){
    ensure_dir(updates_root());std::string text="phase="+p.phase+"\r\nversion="+p.version+"\r\nfrom="+p.from+
        "\r\nstaging="+p.staging+"\r\ntime="+p.time+"\r\n";
    if(!p.detail.empty())text+="detail="+p.detail+"\r\n";
    for(const auto& f:p.files)text+="file="+f+"\r\n";for(const auto& f:p.applied)text+="applied="+f+"\r\n";
    for(const auto& f:p.added)text+="added="+f+"\r\n";for(const auto& f:p.deletes)text+="delete="+f+"\r\n";
    for(const auto& f:p.deleted)text+="deleted="+f+"\r\n";
    if(!p.operation.empty())text+="operation="+p.operation+"\r\noperationExisted="+std::string(p.operationExisted?"1":"0")+"\r\n";
    return write_recovery_ledger_atomic(pending_path(),text);
}
inline std::string now_text(){
    char buffer[64];std::time_t time=std::time(nullptr);std::tm value{};localtime_s(&value,&time);
    std::snprintf(buffer,sizeof(buffer),"%04d-%02d-%02d %02d:%02d:%02d",value.tm_year+1900,value.tm_mon+1,
        value.tm_mday,value.tm_hour,value.tm_min,value.tm_sec);return buffer;
}
inline std::string backup_dir(const std::string& version){return join_path(version_dir(version),"backup");}
inline std::string deleted_backup_dir(const std::string& version){return join_path(backup_dir(version),"deleted");}
inline bool write_ready_plan(const std::string& version,const std::string& from,const std::string&,std::string& error){
    if(!safe_install_rel(version)||version.find_first_of("\\/")!=std::string::npos){error="更新版本目录非法";return false;}
    const std::string staging=join_path(version_dir(version),"staging");if(!staging_verify(staging,error)){error="暂存不完整："+error;return false;}
    std::vector<std::string> files;list_files_rel(staging,"",files);
    files.erase(std::remove(files.begin(),files.end(),std::string("complete.txt")),files.end());
    const auto deletes=staging_deletes(staging);if(files.empty()&&deletes.empty()){error="暂存没有安装或删除项";return false;}
    PendingPlan p;p.phase="ready";p.version=version;p.from=from;p.staging=staging;p.time=now_text();p.files=files;p.deletes=deletes;
    if(!write_pending(p)){error="无法写台账："+pending_path();return false;}return true;
}
inline bool move_file(const std::string& from,const std::string& to){
    return rta::platform::move_or_copy_file(rta::platform::from_utf8(from),rta::platform::from_utf8(to));
}
inline bool restore_backup_atomic(const std::string& backup,const std::string& target){
    if(!path_exists(backup))return false;const auto temp=rta::platform::from_utf8(update_temporary_path(target));
    HANDLE handle=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;CloseHandle(handle);
    bool ok=CopyFileW(rta::platform::from_utf8(backup).c_str(),temp.c_str(),FALSE)!=0;
    if(ok){SetFileAttributesW(temp.c_str(),FILE_ATTRIBUTE_NORMAL);handle=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        ok=handle!=INVALID_HANDLE_VALUE;if(ok){ok=FlushFileBuffers(handle)!=0;CloseHandle(handle);}}
    if(ok)ok=MoveFileExW(temp.c_str(),rta::platform::from_utf8(target).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok){SetFileAttributesW(temp.c_str(),FILE_ATTRIBUTE_NORMAL);DeleteFileW(temp.c_str());}return ok;
}
inline bool install_staging_atomic(const std::string& source,const std::string& target){
    const auto temp=rta::platform::from_utf8(update_temporary_path(target));
    HANDLE handle=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;CloseHandle(handle);
    bool ok=CopyFileW(rta::platform::from_utf8(source).c_str(),temp.c_str(),FALSE)!=0;
    if(ok){handle=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        ok=handle!=INVALID_HANDLE_VALUE;if(ok){ok=FlushFileBuffers(handle)!=0;CloseHandle(handle);}}
    if(ok)ok=MoveFileExW(temp.c_str(),rta::platform::from_utf8(target).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok){SetFileAttributesW(temp.c_str(),FILE_ATTRIBUTE_NORMAL);DeleteFileW(temp.c_str());}return ok;
}
inline std::string recover_transaction(PendingPlan& p,const std::string& reason){
    const std::string inst=install_dir(),bak=backup_dir(p.version),deletedBak=deleted_backup_dir(p.version);std::string failed;
    auto fail=[&](const std::string& rel){failed+=(failed.empty()?"":"; ")+rel;};
    if(!p.operation.empty()){
        const size_t previousFailures=failed.size();
        const bool install=p.operation.rfind("install:",0)==0;
        const bool deleting=p.operation.rfind("delete:",0)==0;
        const std::string rel=(install||deleting)?p.operation.substr(install?8:7):std::string();
        if((!install&&!deleting)||rel.empty())fail("invalid operation");
        else if(!safe_install_rel(rel)||is_protected_rel(rel))fail(rel);
        else if(install){
            const std::string target=join_path(inst,rel),backup=join_path(bak,rel);
            if(p.operationExisted){if(path_exists(backup)&&!restore_backup_atomic(backup,target))fail(rel);}
            else if(path_exists(target)&&!rta::platform::remove_file(rta::platform::from_utf8(target)))fail(rel);
        }else if(path_exists(join_path(deletedBak,rel))&&!restore_backup_atomic(join_path(deletedBak,rel),join_path(inst,rel)))fail(rel);
        // The intent can be the only durable record when a process stops between
        // moving a file and committing applied/added/deleted. Keep it after a
        // failed restore so an unlocked retry cannot falsely report success.
        if(failed.size()==previousFailures){p.operation.clear();p.operationExisted=false;}
    }
    for(auto it=p.applied.rbegin();it!=p.applied.rend();++it){
        if(std::find(p.added.begin(),p.added.end(),*it)!=p.added.end())continue;
        if(!safe_install_rel(*it)||is_protected_rel(*it)||!restore_backup_atomic(join_path(bak,*it),join_path(inst,*it)))fail(*it);}
    for(const auto& rel:p.added){if(!safe_install_rel(rel)||is_protected_rel(rel)){fail(rel);continue;}
        const std::string target=join_path(inst,rel);if(path_exists(target)&&!rta::platform::remove_file(rta::platform::from_utf8(target)))fail(rel);}
    for(auto it=p.deleted.rbegin();it!=p.deleted.rend();++it){if(!safe_install_rel(*it)||is_protected_rel(*it)||
        !restore_backup_atomic(join_path(deletedBak,*it),join_path(inst,*it)))fail(*it);}
    p.phase=failed.empty()?"apply_failed":"rollback_failed";p.time=now_text();p.detail=reason;
    if(!failed.empty())p.detail+="；恢复失败（备份保留）："+failed;write_pending(p);return failed;
}
inline std::string apply_pending_on_startup(){
    PendingPlan p=read_pending();if(!p.exists)return std::string();
    if(p.phase=="applying"){
        const std::string failed=recover_transaction(p,"上次启动在替换中断，已先恢复");if(!failed.empty())return "UPDATE 中断恢复未完成："+failed;
        p.phase="ready";p.applied.clear();p.added.clear();p.deleted.clear();p.operation.clear();if(!write_pending(p))return "UPDATE 无法重置中断台账";
    }
    if(p.phase!="ready")return std::string();
    if(!safe_install_rel(p.version)||p.version.find_first_of("\\/")!=std::string::npos)return "UPDATE 版本目录非法";
    p.staging=join_path(version_dir(p.version),"staging");std::string stagingError;
    if(!staging_verify(p.staging,stagingError)){p.phase="apply_failed";p.detail="启动前暂存校验失败："+stagingError;write_pending(p);return "UPDATE 失败："+p.detail;}
    const std::string inst=install_dir(),bak=backup_dir(p.version),deletedBak=deleted_backup_dir(p.version);
    std::vector<std::string> installs,deletes;std::set<std::string> installSet;int skipped=0;
    for(const auto& rel:p.files){if(!safe_install_rel(rel)){p.phase="apply_failed";p.detail="安装路径非法："+rel;write_pending(p);return "UPDATE 失败："+p.detail;}
        if(is_protected_rel(rel)){++skipped;continue;}if(path_exists(join_path(p.staging,rel))){installs.push_back(rel);installSet.insert(norm_rel2(rel));}}
    for(const auto& rel:p.deletes){if(!safe_install_rel(rel)||installSet.count(norm_rel2(rel))){p.phase="apply_failed";p.detail="删除路径非法或与安装重叠："+rel;write_pending(p);return "UPDATE 失败："+p.detail;}
        if(is_protected_rel(rel)){++skipped;continue;}deletes.push_back(rel);}
    if(installs.empty()&&deletes.empty()){p.phase="apply_failed";p.detail="可安装/删除文件为 0";write_pending(p);return "UPDATE 失败："+p.detail;}
    if(!ensure_dir(bak)||!ensure_dir(deletedBak))return "UPDATE 失败：无法创建备份目录";
    p.phase="applying";p.applied.clear();p.added.clear();p.deleted.clear();p.operation.clear();
    if(!write_pending(p))return "UPDATE 失败：无法写事务初始台账";
    auto abort=[&](const std::string& why){const std::string failed=recover_transaction(p,why);
        return failed.empty()?"UPDATE 失败并已回滚："+why:"UPDATE 失败且恢复未完成："+failed;};
    for(const auto& rel:deletes){const std::string target=join_path(inst,rel);if(!path_exists(target))continue;
        p.operation="delete:"+rel;p.operationExisted=true;if(!write_pending(p))return abort("无法记录删除意图："+rel);
        const std::string backup=join_path(deletedBak,rel);const size_t slash=backup.find_last_of('\\');if(slash!=std::string::npos)ensure_dir(backup.substr(0,slash));
        rta::platform::remove_file(rta::platform::from_utf8(backup));if(!move_file(target,backup))return abort("删除前备份失败："+rel);
        p.deleted.push_back(rel);p.operation.clear();if(!write_pending(p))return abort("删除台账提交失败："+rel);}
    for(const auto& rel:installs){const std::string target=join_path(inst,rel),source=join_path(p.staging,rel);
        const size_t slash=target.find_last_of('\\');if(slash!=std::string::npos&&!ensure_dir(target.substr(0,slash)))return abort("无法创建目标目录："+rel);
        const bool existed=path_exists(target);p.operation="install:"+rel;p.operationExisted=existed;if(!write_pending(p))return abort("无法记录安装意图："+rel);
        if(existed){const std::string backup=join_path(bak,rel);const size_t bs=backup.find_last_of('\\');if(bs!=std::string::npos)ensure_dir(backup.substr(0,bs));
            rta::platform::remove_file(rta::platform::from_utf8(backup));if(!move_file(target,backup))return abort("备份失败："+rel);}
        if(!install_staging_atomic(source,target))return abort("安装失败："+rel);
        p.applied.push_back(rel);if(!existed)p.added.push_back(rel);p.operation.clear();if(!write_pending(p))return abort("安装台账提交失败："+rel);}
    p.phase="applied";p.time=now_text();p.detail="替换 "+std::to_string(p.applied.size())+"，事务删除 "+std::to_string(p.deleted.size())+
        "，跳过受保护 "+std::to_string(skipped);if(!write_pending(p))return abort("更新台账提交失败");
    return "UPDATE 已替换 "+std::to_string(p.applied.size())+" 个文件，删除 "+std::to_string(p.deleted.size())+" 个，备份在 "+bak;
}
inline std::string rollback_pending(const std::string& why){
    PendingPlan p=read_pending();if(!p.exists||(p.phase!="applied"&&p.phase!="applying"&&p.phase!="rollback_failed"))return std::string();
    const std::string failed=recover_transaction(p,why);
    if(failed.empty()){
        p.phase="rolled_back";p.detail=why+"（替换与删除均已恢复）";
        if(!write_pending(p)){
            p.phase="applied";p.detail=why+"（文件已恢复，但回滚状态写入失败）";
            write_pending(p);
            return "UPDATE 回滚状态写入失败："+p.detail;
        }
    }
    report_update_async(p.from,p.version,false,0,failed.empty()?"rolled_back":"rollback_failed");
    return std::string(failed.empty()?"UPDATE 已回滚：":"UPDATE 回滚未完成：")+p.detail;
}

// Reject junctions/symlinks at every component below the installation root.
inline bool asset_path_without_reparse(const std::string& root,const std::string& rel){
    std::string path=root;auto plain=[](const std::string& p){return !rta::platform::is_reparse_point(rta::platform::from_utf8(p));};
    if(!plain(path))return false;size_t begin=0;while(begin<rel.size()){const size_t end=rel.find_first_of("\\/",begin);
        path=join_path(path,rel.substr(begin,end==std::string::npos?end:end-begin));if(!plain(path))return false;
        if(end==std::string::npos)break;begin=end+1;}return true;
}
inline bool migrate_application_update_assets(PendingPlan& p,std::string& error){
    const std::string inst=install_dir();if(!safe_install_rel(p.version)||p.version.find_first_of("\\/")!=std::string::npos){error="更新版本目录非法";return false;}
    const std::string stageRel="updates/"+p.version+"/staging/",backupRel="updates/"+p.version+"/backup/";
    std::vector<const ApplicationUpdateAsset*> todo;
    for(const auto& asset:kApplicationUpdateAssets){
        if(!asset_path_without_reparse(inst,asset.path)||!asset_path_without_reparse(inst,stageRel+asset.path)||
           !asset_path_without_reparse(inst,backupRel+asset.path)){error=std::string("应用资源路径含链接：")+asset.path;return false;}
        std::string hash,e;if(sha256_file(join_path(inst,asset.path),hash,e)&&hex_equals_ci(hash,asset.sha256))continue;
        if(!sha256_file(join_path(inst,stageRel+asset.path),hash,e)||!hex_equals_ci(hash,asset.sha256)){error=std::string("应用资源缺失或校验失败：")+asset.path;return false;}
        todo.push_back(&asset);
    }
    for(const auto* asset:todo){const std::string rel=norm_rel2(asset->path),target=join_path(inst,asset->path),
        backup=join_path(inst,backupRel+asset->path),source=join_path(inst,stageRel+asset->path);const bool existed=path_exists(target);
        if(!ensure_dir(target.substr(0,target.find_last_of("\\/")))||!ensure_dir(backup.substr(0,backup.find_last_of("\\/")))){error="无法创建应用资源目录";return false;}
        if(existed&&!path_exists(backup)&&!rta::platform::copy_file(rta::platform::from_utf8(target),rta::platform::from_utf8(backup),true)){error="应用资源备份失败";return false;}
        if(!rta::platform::copy_file(rta::platform::from_utf8(source),rta::platform::from_utf8(target),false)){error="应用资源迁移失败";return false;}
        std::string hash,e;if(!sha256_file(target,hash,e)||!hex_equals_ci(hash,asset->sha256)){error="应用资源落盘校验失败";return false;}
        if(std::find(p.applied.begin(),p.applied.end(),rel)==p.applied.end())p.applied.push_back(rel);
        if(!existed&&std::find(p.added.begin(),p.added.end(),rel)==p.added.end())p.added.push_back(rel);
        if(!write_pending(p)){error="应用资源迁移台账写入失败";return false;}}
    return true;
}
inline std::string confirm_on_startup(){
    PendingPlan p=read_pending();if(!p.exists)return std::string();
    if(p.phase=="applied"){
        if(cmp_versions(p.version,current_version())==0){std::string assetError;
            if(!migrate_application_update_assets(p,assetError)){p.detail=assetError;write_pending(p);return "UPDATE 未完成："+assetError;}
            p.phase="done";p.time=now_text();if(!write_pending(p))return "UPDATE 确认台账写入失败";
            rmtree(join_path(version_dir(p.version),"staging"));report_update_async(p.from,p.version,true,0,"");
            return "UPDATE ok "+p.from+" -> "+p.version+"（"+p.detail+"）";}
        return "UPDATE 台账版本与当前进程不符";
    }
    if(p.phase=="apply_failed"){report_update_async(p.from,p.version,false,0,"apply_failed");return "UPDATE 上次替换失败："+p.detail;}
    if(p.phase=="rolled_back")return "UPDATE 上次更新失败已回滚："+p.detail;
    if(p.phase=="rollback_failed")return "UPDATE 上次回滚未完成（备份保留）："+p.detail;
    if(p.phase=="ready"||p.phase=="applying")return "UPDATE 有已下载但尚未安装的更新";return std::string();
}
inline std::string state_json(){
    const PendingPlan p=read_pending();auto& shared=state();bool staged=false;std::string version,staging;
    {std::lock_guard<std::mutex> lock(shared.mtx);staged=shared.staged;version=shared.dlVersion;staging=shared.stagingDir;}
    std::string j="{\"ok\":true,\"current\":\""+json_escape(current_version())+"\",\"installDir\":\""+json_escape(install_dir())+"\"";
    j+=",\"skipVersion\":\""+json_escape(skip_version())+"\",\"staged\":"+(staged?"true":"false");
    j+=",\"stagedVersion\":\""+json_escape(version)+"\",\"stagingDir\":\""+json_escape(staging)+"\"";
    j+=",\"pendingExists\":"+std::string(p.exists?"true":"false")+",\"pendingPhase\":\""+json_escape(p.phase)+"\"";
    j+=",\"pendingVersion\":\""+json_escape(p.version)+"\",\"pendingFrom\":\""+json_escape(p.from)+"\"";
    j+=",\"pendingTime\":\""+json_escape(p.time)+"\",\"pendingDetail\":\""+json_escape(p.detail)+"\"";
    j+=",\"pendingFiles\":"+std::to_string((long long)p.applied.size())+",\"pendingDeletes\":"+std::to_string((long long)p.deleted.size());
    j+=",\"backupDir\":\""+json_escape(backup_dir(p.version))+"\"}";return j;
}
inline bool request_launcher_exit(){return rta::platform::request_launcher_exit();}
inline std::string powershell_unused_marker(){return std::string();}

} // namespace rta::update
