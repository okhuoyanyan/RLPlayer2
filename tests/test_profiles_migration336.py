"""Execute production profile migration, updater protection and release hygiene.

All writes stay inside a fresh private evidence directory. No player, device,
network server or real user profile is opened.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

PROBE = r'''
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include "tools/launcher_profile_migration.h"
#include "net/update.h"
#include "net/update_apply.h"
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>
namespace fs=std::filesystem;
int checks=0;
void require(bool value,const char* label){++checks;if(!value)throw std::runtime_error(label);std::cout<<"PASS "<<label<<'\n';}
void put(const fs::path&p,const std::string&b){fs::create_directories(p.parent_path());std::ofstream(p,std::ios::binary)<<b;}
std::string get(const fs::path&p){std::ifstream f(p,std::ios::binary);return {(std::istreambuf_iterator<char>(f)),{}};}
int main(int argc,char**argv){try{
 if(argc!=2)return 2;const auto root=fs::u8path(argv[1]);const auto bank=rta::launcher_profiles::filename;
 const auto payload=get(root/"payload.json");require(!payload.empty(),"private four-group sixteen-EQ document supplied");
 auto area=root/"migration-numeric",current=area/"RLPlayer-3.3.7-win64",latest=area/"RLPlayer-3.3.6-win64";
 fs::create_directories(current);put(latest/bank,payload);put(area/"RLPlayer-3.3.5-win64"/bank,"OLDER");put(area/"RLPlayer-3.10.0-win64"/bank,"FUTURE");put(area/"arbitrary-folder"/bank,"UNRELATED");
 auto migrated=rta::launcher_profiles::migrate(current,"3.3.7");
 require(migrated.status=="copied"&&migrated.source==latest/bank,"launcher selects newest explicit nonfuture release sibling");
 require(get(current/bank)==payload&&get(latest/bank)==payload,"migration preserves exact source and destination bytes");
 put(latest/bank,"LATER-OLD-EDIT");require(rta::launcher_profiles::migrate(current,"3.3.7").status=="existing"&&get(current/bank)==payload,"existing new-version bank is never overwritten");
 area=root/"migration-unicode";current=area/fs::u8path("播放器");fs::create_directories(current);put(area/"RLPlayer-3.3.6-win64"/bank,payload);
 require(rta::launcher_profiles::migrate(current,"3.3.7").status=="copied"&&get(current/bank)==payload,"renamed Unicode installation keeps the complete bank");
 area=root/"migration-previous";current=area/"RLPlayer-3.3.6-win64";fs::create_directories(current);put(area/"RLPlayer-3.3.6-win64.previous-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"/bank,payload);
 auto same=rta::launcher_profiles::migrate(current,"3.3.6");
 if(same.status!="copied"){std::this_thread::sleep_for(std::chrono::milliseconds(80));same=rta::launcher_profiles::migrate(current,"3.3.6");}
 require(same.status=="copied"&&get(current/bank)==payload,"same-version reinstall previous directory remains recoverable");
 area=root/"migration-empty-current";current=area/"RLPlayer-3.3.7-win64";put(current/bank,"");put(area/"RLPlayer-3.3.6-win64"/bank,payload);
 require(rta::launcher_profiles::migrate(current,"3.3.7").status=="existing"&&get(current/bank).empty(),"explicit empty current bank is preserved as user state");
 area=root/"migration-reject";current=area/"RLPlayer-3.3.7-win64";fs::create_directories(current);put(area/"arbitrary-folder"/bank,payload);put(area/"RLPlayer-4.0.0-win64"/bank,payload);
 require(rta::launcher_profiles::migrate(current,"3.3.7").status=="missing"&&!fs::exists(current/bank),"unrelated and future profiles are not imported");
 put(area/"RLPlayer-3.3.6-win64"/bank,"");put(area/"RLPlayer-3.3.5-win64"/bank,payload);
 require(rta::launcher_profiles::migrate(current,"3.3.7").status=="failed"&&!fs::exists(current/bank),"unreadable newest bank never silently falls back to stale data");
 area=root/"migration-race";current=area/"RLPlayer-3.3.7-win64";fs::create_directories(current);put(area/"RLPlayer-3.3.6-win64"/bank,payload);
 rta::launcher_profiles::Result first,second;std::thread a([&]{first=rta::launcher_profiles::migrate(current,"3.3.7");}),b([&]{second=rta::launcher_profiles::migrate(current,"3.3.7");});a.join();b.join();
 require(get(current/bank)==payload&&(first.status=="copied"||second.status=="copied")&&(first.status=="existing"||second.status=="existing"),"concurrent launchers publish once without overwrite or partial JSON");
 using namespace rta::update;
 for(const auto& rel:std::vector<std::string>{bank,"nested/RLPLAYER-WORKBENCH-PROFILES.JSON","rlplayer-settings.conf","rlplayer-playlist.txt","rlplayer-favs.json"}){
  require(is_protected_rel(rel),"new bank and existing user data remain protected");
  ManifestFile file;file.path=rel;file.size=4;file.sha256=std::string(64,'a');file.objectSha256=std::string(64,'b');file.objectSize=20;
  const auto plan=plan_incremental({file},root.string(),{rel});
  require(plan.ok&&plan.toDownload==0&&plan.protectedSkipped==1&&plan.deletes.empty()&&plan.protectedDeletes==1,"malicious manifest replacement and deletion are excluded");
 }
 require(!is_protected_rel(std::string(bank)+".backup"),"protection uses an exact basename");
 auto installation=root/"update-signed";fs::create_directories(installation);_putenv_s("RLPLAYER_UPDATE_DIR",installation.string().c_str());
 auto stage=fs::u8path(version_dir("9.9.9"))/"staging";put(installation/bank,payload);put(installation/"app.bin","OLD");put(stage/bank,"EVIL");put(stage/"app.bin","NEW");
 std::string marker="version=9.9.9\nfile=app.bin\t3\t"+sha256_hex(std::string("NEW"))+"\nfile="+bank+"\t4\t"+sha256_hex(std::string("EVIL"))+"\n";
 put(fs::u8path(staging_marker_path(stage.string())),marker);PendingPlan pending;pending.phase="ready";pending.version="9.9.9";pending.from="3.3.6";pending.files={"app.bin",bank};write_pending(pending);apply_pending_on_startup();
 require(read_pending().phase=="apply_failed"&&get(installation/bank)==payload&&get(installation/"app.bin")=="OLD","signed staging rejects bank replacement before any installation");
 installation=root/"update-legacy";fs::create_directories(installation);_putenv_s("RLPLAYER_UPDATE_DIR",installation.string().c_str());stage=fs::u8path(version_dir("9.9.9"))/"staging";
 put(installation/bank,payload);put(installation/"app.bin","OLD");put(stage/bank,"EVIL");put(stage/"app.bin","NEW");put(fs::u8path(staging_marker_path(stage.string())),std::string("file=app.bin\t3\nfile=")+bank+"\t4\ndelete="+bank+"\n");
 pending.phase="ready";pending.files={"app.bin",bank};pending.deletes={bank};write_pending(pending);apply_pending_on_startup();auto applied=read_pending();
 require(applied.phase=="applied"&&get(installation/"app.bin")=="NEW"&&get(installation/bank)==payload,"legacy install ignores protected bank overwrite and delete");
 require(applied.applied==std::vector<std::string>{"app.bin"}&&applied.deleted.empty(),"bank never enters installation or deletion rollback ledgers");
 require(recover_transaction(applied,"offline profile gate").empty()&&get(installation/"app.bin")=="OLD"&&get(installation/bank)==payload,"normal recovery preserves current bank byte for byte");
 pending=read_pending();pending.applied={bank};pending.added={bank};pending.deleted={bank};put(fs::u8path(backup_dir("9.9.9"))/bank,"FORGED");
 require(!recover_transaction(pending,"forged protected ledger").empty()&&get(installation/bank)==payload,"forged rollback cannot overwrite or delete the current bank");
 _putenv_s("RLPLAYER_UPDATE_DIR","");std::cout<<"RESULT "<<checks<<" pass 0 fail\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<" after "<<checks<<" checks\n";return 1;}}
'''


def main() -> int:
    parser=argparse.ArgumentParser()
    parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1])
    parser.add_argument('--build',type=Path)
    parser.add_argument('--compiler',default='C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe')
    parser.add_argument('--evidence-dir',type=Path)
    args=parser.parse_args();source=args.source.resolve()
    base=(args.evidence_dir or source/'build-pro336'/'profiles-migration-evidence').resolve();base.mkdir(parents=True,exist_ok=True)
    # The probe creates "...previous-<32 hex>" siblings; under the safe-gate evidence
    # directory that path exceeds Win32 MAX_PATH (263 chars) and the migration fixture
    # silently fails to create its bank. Keep the run root short when evidence is deep.
    run_parent=base if len(str(base))<110 else Path(tempfile.gettempdir())
    run=Path(tempfile.mkdtemp(prefix='profiles336-',dir=run_parent))
    build=args.build or next((p for p in [source/'build-pro336',source/'build336',source/'build-backend336-clang'] if (p/'generated/net/update_assets_hashes.h').is_file()),None)
    if build is None:raise RuntimeError('supply --build with generated/net/update_assets_hashes.h')
    peq={'bypass':0,'preamp':-3.25,'bands':[{'i':i+1,'on':True,'type':'PK','freq':20*1000**(i/19),'gain':(i-10)/10,'q':1.41} for i in range(20)]}
    payload={'version':1,'active':'D','slots':[{'id':c,'name':'私人调音 '+c,'settings':{'filter':'poly-sinc-long-lp','rate':str(88200*(i+1))},'peq':peq,'eqId':str(i+1),'eqName':'Private '+c,'migrated':True} for i,c in enumerate('ABCD')], 'eqs':[{'id':str(i),'name':'耳机校准 '+str(i),'source':'custom','productId':'','presetId':'','link':'','empty':False,'peq':peq} for i in range(1,17)]}
    original=(json.dumps(payload,ensure_ascii=False,indent=2)+'\r\n').encode('utf-8');(run/'payload.json').write_bytes(original)
    cpp=run/'test_profiles_migration336.cpp';cpp.write_text(PROBE,encoding='utf-8');exe=run/'test_profiles_migration336.exe'
    result=subprocess.run([args.compiler,'-std=c++23','-O0','-D_CRT_SECURE_NO_WARNINGS','-I'+str(source),'-I'+str(source/'src'),'-I'+str(build/'generated'),str(cpp),'-o',str(exe),'-lwinhttp','-lws2_32','-lbcrypt'],capture_output=True,timeout=120)
    (run/'compile.log').write_bytes(result.stdout+result.stderr)
    if result.returncode:raise RuntimeError('probe compile failed: '+str(run/'compile.log'))
    result=subprocess.run([str(exe),str(run)],capture_output=True,timeout=45,env={**os.environ,'RTA_NO_DEVICE':'1','RTA_NET_TESTS':'0'})
    (run/'probe.log').write_bytes(result.stdout+result.stderr);match=re.search(rb'RESULT (\d+) pass 0 fail',result.stdout)
    if result.returncode or match is None:raise RuntimeError('production migration/updater failed: '+str(run/'probe.log'))
    checks=int(match[1]);reports=[]
    for label,nested in [('root-bank-purged',False),('nested-bank-refused',True)]:
        build_root=run/label;out=build_root/'stage';out.mkdir(parents=True);target=out/('nested' if nested else '')/'rlplayer-workbench-profiles.json';target.parent.mkdir(exist_ok=True);target.write_bytes(original);(out/'program.bin').write_bytes(b'KEEP');(build_root/'outside-sentinel').write_bytes(b'OUTSIDE')
        quote=lambda p:"'"+str(p).replace("'","''")+"'"
        ps="$ErrorActionPreference='Stop'\n. "+quote(source/'tools/package_paths.ps1')+"\n$B="+quote(build_root)+"\n$out="+quote(out)+"\nfunction Get-SafeCleanupDirectory([string]$Target,[string]$Parent){return Resolve-ReleaseCleanupDirectory -BuildDir $B -Target $Target -Parent $Parent}\nfunction Fail([string]$Message){throw $Message}\n$source=[IO.File]::ReadAllText("+quote(source/'tools/make_dist.ps1')+")\n$first=$source.IndexOf('$devStateDirs =');$last=$source.IndexOf(\"Write-Host 'release hygiene:\",$first);if($first -lt 0 -or $last -le $first){throw 'production hygiene block missing'}\n& ([scriptblock]::Create($source.Substring($first,$last-$first)))\n"
        script=build_root/'hygiene.ps1';script.write_text(ps,encoding='utf-8-sig')
        clean=subprocess.run(['C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-NonInteractive','-File',str(script)],capture_output=True,timeout=45);(build_root/'hygiene.log').write_bytes(clean.stdout+clean.stderr)
        passed=(clean.returncode!=0 and target.read_bytes()==original) if nested else (clean.returncode==0 and not target.exists())
        passed=passed and (out/'program.bin').read_bytes()==b'KEEP' and (build_root/'outside-sentinel').read_bytes()==b'OUTSIDE'
        if not passed:raise AssertionError(label+' failed: '+str(build_root/'hygiene.log'))
        reports.append({'case':label,'pass':True});checks+=1
    copied=run/'migration-numeric/RLPlayer-3.3.7-win64/rlplayer-workbench-profiles.json'
    decoded=json.loads(copied.read_bytes());assert copied.read_bytes()==original and len(decoded['slots'])==4 and len(decoded['eqs'])==16
    report={'pass':True,'checks':checks,'failures':0,'groups':4,'savedEq':16,'exactBytes':True,'sha256':hashlib.sha256(original).hexdigest(),'hygiene':reports,'run':str(run),'actualAudioFrames':0}
    (run/'results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8');print(json.dumps(report));return 0


if __name__=='__main__':raise SystemExit(main())
