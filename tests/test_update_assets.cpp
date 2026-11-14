#include "net/update.h"
#include "net/update_apply.h"
#include <filesystem>
#include <iostream>
using namespace rta::update;
static int checks=0;
static void require(bool v,const char* msg){++checks;if(!v)throw std::runtime_error(msg);}
int main(){try{
 const std::string root=join_path(rta::platform::temp_dir_utf8(),"rl-update-assets-"+std::to_string(GetCurrentProcessId()));
 ensure_dir(root); _putenv_s("RLPLAYER_UPDATE_DIR",root.c_str());
 const char* codes[]={"ar","en","es","fr","ja","ru"};
 auto asset=[&](const std::string& base,const char* code){return join_path(base,std::string("data\\i18n\\")+code+".json");};
 auto prepare=[&](){rmtree(root);ensure_dir(root);ensure_dir(join_path(root,"data\\i18n"));
  const auto st=join_path(version_dir(current_version()),"staging");ensure_dir(join_path(st,"data\\i18n"));
  for(auto c:codes)require(CopyFileW(rta::platform::from_utf8(asset(UPDATE_ASSET_SOURCE,c)).c_str(),rta::platform::from_utf8(asset(st,c)).c_str(),FALSE)!=0,"fixture copy");
  write_text_file(join_path(root,"rlplayer-settings.conf"),"fake-user-settings");write_text_file(join_path(root,"rlplayer-queue.txt"),"fake-user-queue");
  PendingPlan p;p.phase="applied";p.version=current_version();p.from="2.0.0";p.staging=st;write_pending(p);return st;};
 auto st=prepare();
 confirm_on_startup();require(read_pending().phase=="done","old launcher migration must complete");
 for(auto c:codes){require(read_text_file(asset(root,c))==read_text_file(asset(UPDATE_ASSET_SOURCE,c)),"all six assets migrated");require(!is_protected_rel(std::string("data/i18n/")+c+".json"),"fixed asset allowed");}
 require(read_text_file(join_path(root,"rlplayer-settings.conf"))=="fake-user-settings","settings unchanged");require(read_text_file(join_path(root,"rlplayer-queue.txt"))=="fake-user-queue","queue unchanged");require(!is_dir(st),"successful staging cleaned");
 require(is_protected_rel("data/i18n/custom.json")&&is_protected_rel("data/library.db"),"other data protected");
 st=prepare();write_text_file(asset(st,"en"),"tampered");auto msg=confirm_on_startup();require(read_pending().phase=="applied","tamper must not confirm");require(is_dir(st),"tamper preserves staging");require(msg.find("en.json")!=std::string::npos,"explicit failure path");
 require(!path_exists(asset(root,"ar")),"preflight must prevent partial migration");
 st=prepare();DeleteFileW(rta::platform::from_utf8(asset(st,"fr")).c_str());confirm_on_startup();require(read_pending().phase=="applied"&&is_dir(st),"missing resource refuses success");
 CopyFileW(rta::platform::from_utf8(asset(UPDATE_ASSET_SOURCE,"fr")).c_str(),rta::platform::from_utf8(asset(st,"fr")).c_str(),FALSE);confirm_on_startup();require(read_pending().phase=="done","retry succeeds");
 st=prepare();for(auto c:codes)CopyFileW(rta::platform::from_utf8(asset(st,c)).c_str(),rta::platform::from_utf8(asset(root,c)).c_str(),FALSE);rmtree(st);confirm_on_startup();require(read_pending().phase=="done","correct targets require no staging");
 st=prepare();write_text_file(asset(root,"en"),"old app resource");confirm_on_startup();require(read_text_file(join_path(backup_dir(current_version()),"data\\i18n\\en.json"))=="old app resource","old resource backed up");auto ledger=read_pending();require(std::find(ledger.applied.begin(),ledger.applied.end(),"data\\i18n\\en.json")!=ledger.applied.end(),"backup included in ledger");

 st=prepare();write_text_file(asset(root,"en"),"old app resource");auto pending=read_pending();std::string err;
 require(migrate_application_update_assets(pending,err),"migration helper success");rollback_pending("test rollback");
 require(read_text_file(asset(root,"en"))=="old app resource","rollback restores prior app resource");require(!path_exists(asset(root,"ar")),"rollback removes added app resource");
 st=prepare();auto outside=root+"-outside";ensure_dir(outside);auto link=join_path(root,"data\\i18n");rmtree(link);
 require(CreateSymbolicLinkW(rta::platform::from_utf8(link).c_str(),rta::platform::from_utf8(outside).c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY|0x2)!=0,"create isolated symlink fixture");
 confirm_on_startup();require(read_pending().phase=="applied","symlink target rejected");require(!path_exists(asset(outside,"en")),"symlink cannot write outside");
 RemoveDirectoryW(rta::platform::from_utf8(link).c_str());rmtree(outside);
 st=prepare();auto original=read_pending();original.staging=root+"-untrusted";write_pending(original);confirm_on_startup();require(read_pending().phase=="done","ignore untrusted pending staging path");

 // A distribution must never install or roll back per-track manual artwork.
 st=prepare();const std::string cacheDir=join_path(root,"trackcovercache");ensure_dir(cacheDir);ensure_dir(join_path(st,"trackcovercache"));
 const std::string manifestRel=R"(trackcovercache\0123456789abcdef.jpg.source.json)",blobRel=R"(trackcovercache\0123456789abcdef.sha.blob)",newRel=R"(trackcovercache\new.blob)";
 write_text_file(join_path(root,manifestRel),"user-manual-source");write_text_file(join_path(root,blobRel),"user-manual-image");write_text_file(join_path(root,"app.bin"),"old-app");
 const std::vector<std::string> planned={"app.bin",manifestRel,blobRel,newRel};std::string marker;
 for(const auto& rel:planned){write_text_file(join_path(st,rel),"package-content");marker+="file="+rel+"\t15\n";}
 write_text_file(staging_marker_path(st),marker);auto plan=read_pending();plan.phase="ready";plan.files=planned;write_pending(plan);apply_pending_on_startup();
 require(read_pending().phase=="applied"&&read_text_file(join_path(root,"app.bin"))=="package-content","actual install replaces application file");
 require(read_text_file(join_path(root,manifestRel))=="user-manual-source"&&read_text_file(join_path(root,blobRel))=="user-manual-image","actual install preserves track manual manifest and image bytes");
 require(!path_exists(join_path(root,newRel)),"package cannot add files inside track artwork state");
 auto installed=read_pending();require(installed.applied.size()==1&&installed.applied[0]=="app.bin","protected track artwork is absent from install rollback ledger");
 rollback_pending("private artwork preservation regression");require(read_pending().phase=="rolled_back"&&read_text_file(join_path(root,"app.bin"))=="old-app","normal rollback restores application file");
 require(read_text_file(join_path(root,manifestRel))=="user-manual-source"&&read_text_file(join_path(root,blobRel))=="user-manual-image","normal rollback leaves track artwork unchanged");
 require(is_protected_rel("TRACKCOVERCACHE/nested/image.blob")&&is_protected_rel("trackcovercache"),"track cache protection is recursive and case/slash independent");
 require(!is_protected_rel("trackcovercache-other/file.bin"),"protection requires a complete directory component");
 auto forged=read_pending();forged.phase="applied";forged.applied={manifestRel};forged.added={blobRel};ensure_dir(join_path(backup_dir(forged.version),"trackcovercache"));write_text_file(join_path(backup_dir(forged.version),manifestRel),"wrong-backup-source");write_pending(forged);rollback_pending("untrusted artwork ledger");
 require(read_pending().phase=="rollback_failed","rollback refuses a ledger that names protected artwork");
 require(read_text_file(join_path(root,manifestRel))=="user-manual-source"&&read_text_file(join_path(root,blobRel))=="user-manual-image","forged rollback cannot replace or delete user track artwork");
 // Execute the exact production packaging cleanup block in a confined fake stage.
 const std::string build=join_path(root,"package-build"),out=join_path(build,"stage");ensure_dir(join_path(out,"trackcovercache"));write_text_file(join_path(out,"trackcovercache/user.blob"),"developer-cache");write_text_file(join_path(out,"program.bin"),"keep-program");write_text_file(join_path(build,"outside-sentinel"),"keep-outside");
 const std::string script=join_path(root,"test-package-cleanup.ps1");
 auto psquote=[](std::string x){size_t at=0;while((at=x.find('\'',at))!=std::string::npos){x.insert(at,"'");at+=2;}return "'"+x+"'";};
 const std::string tools=join_path(UPDATE_ASSET_SOURCE,"tools"),make=join_path(tools,"make_dist.ps1");
 std::string ps="$ErrorActionPreference='Stop'\n. "+psquote(join_path(tools,"package_paths.ps1"))+"\n$B="+psquote(build)+"\n$out="+psquote(out)+"\nfunction Get-SafeCleanupDirectory([string]$Target,[string]$Parent){return Resolve-ReleaseCleanupDirectory -BuildDir $B -Target $Target -Parent $Parent}\n";
 ps+="$source=[IO.File]::ReadAllText("+psquote(make)+")\n$first=$source.IndexOf('$devStateDirs =');$last=$source.IndexOf('$devStateGlobs =',$first);if($first -lt 0 -or $last -le $first){throw 'production cleanup block missing'}\n& ([scriptblock]::Create($source.Substring($first,$last-$first)))\n";
 write_text_file(script,ps);const auto cleanup=rta::platform::run_hidden("C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -NonInteractive -File \""+script+"\"",true);
 require(cleanup.started&&cleanup.exit_code==0,"production packaging cleanup executes successfully in private stage");
 require(!path_exists(join_path(out,"trackcovercache")),"distribution cleanup excludes developer track artwork cache");
 require(read_text_file(join_path(out,"program.bin"))=="keep-program"&&read_text_file(join_path(build,"outside-sentinel"))=="keep-outside","packaging cleanup preserves program files and out-of-stage sentinel");

 rmtree(root);_putenv_s("RLPLAYER_UPDATE_DIR","");std::cout<<"PASS "<<checks<<" / FAIL 0\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<" after "<<checks<<" checks\n";return 1;}}
