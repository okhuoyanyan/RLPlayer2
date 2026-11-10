// SPDX-License-Identifier: GPL-3.0-or-later
// Offline transaction/ownership tests. No app, network, audio endpoint or user profile.
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <vector>
#include <string>
#include <chrono>
#include "net/update_check.h"
#include "net/profile_artwork_migration.h"
namespace fs=std::filesystem;
using namespace rta::net::artwork_migration;
static int passed=0,failed=0;
static void check(bool v,const char* n){printf("%s %s\n",v?"PASS":"FAIL",n);v?++passed:++failed;}
static void write(const fs::path&p,const std::string&s){fs::create_directories(p.parent_path());std::ofstream f(p,std::ios::binary);f.write(s.data(),s.size());}
static std::string read(const fs::path&p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static std::string hash(const std::string&s){return rta::update::sha256_hex(s);}
static std::string fnv(const std::string&s){uint64_t h=1469598103934665603ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}char b[20];snprintf(b,sizeof b,"%016llx",(unsigned long long)h);return b;}
static std::string image(bool alt=false){
 // Valid synthetic 8x8 RGB PNG with a tEXt chunk; no copyrighted media.
 const char* hex=alt?"89504e470d0a1a0a0000000d49484452000000080000000808020000004b6d29dc000000d349444154789c01c80037ff00012a532049723f68915e87b07da6cf9cc5eebbe40dda032c00123b64315a835079a26f98c18eb7e0add6ffccf51eeb143d00234c75426b94618ab380a9d29fc8f1bee710dd062ffc254e00345d86537ca5729bc491bae3b0d902cff821ee17400d365f00456e97648db683acd5a2cbf4c1ea13e00932ff28511e477000567fa8759ec794bde6b3dc05d2fb24f11a431039622f5881006790b986afd8a5cef7c4ed16e30c35022b54214a734069920078a1ca97c0e9b6df08d5fe27f41d46133c65325b84517aa3b6d36081a9416bbe0000000049454e44ae426082":"89504e470d0a1a0a0000000d49484452000000080000000808020000004b6d29dc000000d349444154789c01c80037ff000029521f48713e67905d86af7ca5ce9bc4edbae30cd9022b00113a633059824f78a16e97c08db6dfacd5fecbf41dea133c00224b74416a936089b27fa8d19ec7f0bde60fdc052efb244d00335c85527ba4719ac390b9e2afd801cef720ed163f0c355e00446d96638cb582abd4a1caf3c0e912df0831fe27501d466f00557ea7749dc693bce5b2db04d1fa23f019420f38612e578000668fb885aed7a4cdf6c3ec15e20b34012a532049723f68910077a0c996bfe8b5de07d4fd26f31c45123b64315a835079a26bd35fc1cf2d339e0000000049454e44ae426082";
 std::string s;for(size_t i=0;hex[i];i+=2)s.push_back((char)strtoul(std::string(hex+i,2).c_str(),nullptr,16));return s;
}
static std::string typed(const fs::path&profile,const std::string&key,const std::string&bytes,const std::string&source="manual"){
 const std::string slug=key.rfind("track:",0)==0?key.substr(6):key,leaf=slug+"."+hash(bytes)+".blob";
 const auto dir=profile/(key.rfind("track:",0)==0?"trackcovercache":"covercache");
 write(dir/leaf,bytes);
 std::string j="{\"version\":1,\"key\":\""+key+"\",\"source\":\""+source+"\",\"blob\":\""+leaf+"\",\"sha256\":\""+hash(bytes)+"\",\"bytes\":"+std::to_string(bytes.size())+"}";
 write(dir/(slug+".jpg.source.json"),j);return j;
}
static void index(const fs::path&p,const std::string&file,const std::string&album,const std::string&artist){
 write(p/"rlplayer-index.tsv","#libIndexV\t5\n"+file+"\t0\t1\t"+album+"\t"+artist+"\t"+artist+"\tTitle\t1\t1\t2026\t44100\t16\t2\t10\t0\t\t\t0\t0\t0\n");
}
int main(int argc,char**argv){
 const fs::path base=argc>1?fs::u8path(argv[1]):fs::temp_directory_path()/("rlplayer-artwork-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 if(fs::exists(base)){printf("fresh evidence directory required\n");return 2;}fs::create_directories(base);
 const auto current=base/"RLPlayer-3.3.1-win64",old=base/"RLPlayer-3.2.0-win64",newer=base/"RLPlayer-3.3.0-win64",badname=base/"RLPlayer-trap-win64";
 fs::create_directories(current);fs::create_directories(old);fs::create_directories(newer);
 const std::string img=image(),key="1111111111111111",track="C:\\Private\\one.flac",rawhash=fnv(track);
 const std::string jpgmanifest=typed(old,key,img);typed(old,"track:"+rawhash,img);
 write(old/"covercache/ignored.none","negative");write(old/"rlplayer-meta/llm-usage.json","private");
 write(old/"rlplayer-settings.conf","private");write(old/"rlplayer-llm.conf","secret");
 const std::string metadata="{\"path\":\"C:\\\\Private\\\\one.flac\",\"cover\":\"covers/"+rawhash+".jpg\",\"fields\":{\"album\":{\"value\":\"Release\",\"source\":\"user\"}}}";
 write(old/"rlplayer-meta/tracks"/(rawhash+".json"),metadata);write(old/"rlplayer-meta/covers"/(rawhash+".jpg"),img);
 auto m=migrate_profile_artwork(current.string());
 check(read(current/"covercache"/(key+".jpg.source.json"))==jpgmanifest,"typed manifest migrates with exact bytes");
 check(read(current/"covercache"/(key+"."+hash(img)+".blob"))==img,"typed blob exact and paired");
 check(read(current/"trackcovercache"/(rawhash+".jpg.source.json"))==read(old/"trackcovercache"/(rawhash+".jpg.source.json")),"track typed artwork migrates");
 check(read(current/"rlplayer-meta/tracks"/(rawhash+".json"))==metadata,"sparse user metadata preserved exactly");
 check(read(current/"rlplayer-meta/covers"/(rawhash+".jpg"))==img,"associated Meta cover migrated");
 check(!fs::exists(current/"covercache/ignored.none")&&!fs::exists(current/"rlplayer-meta/llm-usage.json")&&!fs::exists(current/"rlplayer-settings.conf")&&!fs::exists(current/"rlplayer-llm.conf"),"negative caches usage configs and secrets excluded");
 auto repeat=migrate_profile_artwork(current.string());check(repeat.typedRecords==0&&repeat.metaRecords==0,"idempotent startup makes no duplicate records");
 const std::string second="2222222222222222";typed(old,second,img);write(current/"covercache"/(second+".jpg.source.json"),"{\"source\":\"reset\"}");
 migrate_profile_artwork(current.string());check(read(current/"covercache"/(second+".jpg.source.json"))=="{\"source\":\"reset\"}","current declaration even reset/malformed never overwritten");
 const std::string multi="3333333333333333";typed(old,multi,img);std::string alternate=image(true);typed(newer,multi,alternate);
 migrate_profile_artwork(current.string());check(read(current/"covercache"/(multi+"."+hash(alternate)+".blob"))==alternate,"newer sibling wins over older sibling");
 const std::string broken="4444444444444444";auto brokenj=typed(old,broken,img);fs::remove(old/"covercache"/(broken+"."+hash(img)+".blob"));
 migrate_profile_artwork(current.string());check(!fs::exists(current/"covercache"/(broken+".jpg.source.json")),"missing blob cannot publish a declaration");
 write(old/"covercache"/(broken+"."+hash(img)+".blob"),img);migrate_profile_artwork(current.string());check(read(current/"covercache"/(broken+".jpg.source.json"))==brokenj,"missing-source failure is retryable");
 // Meta reset must block restoration of bytes, even though the raw-path hash differs from its normalized track key.
 const std::string resetpath="C:\\Private\\RESET.flac",resetraw=fnv(resetpath),resetkey=fnv("c:\\private\\reset.flac");
 write(current/"trackcovercache"/(resetkey+".jpg.source.json"),"{\"version\":1,\"key\":\"track:"+resetkey+"\",\"source\":\"reset\"}");
 write(old/"rlplayer-meta/tracks"/(resetraw+".json"),"{\"path\":\"C:\\\\Private\\\\RESET.flac\",\"cover\":\"covers/"+resetraw+".jpg\",\"fields\":{}}");
 write(old/"rlplayer-meta/covers"/(resetraw+".jpg"),img);migrate_profile_artwork(current.string());
 check(!fs::exists(current/"rlplayer-meta/covers"/(resetraw+".jpg")),"current track reset cannot resurrect a Meta cover");
 // Incomplete publication can leave an orphan blob, but the declaration must stay absent and retry must complete.
 const std::string retrykey="5555555555555555";typed(old,retrykey,img);
 fs::create_directories(current/"covercache"/(retrykey+".jpg.source.json"));
 migrate_profile_artwork(current.string());check(fs::is_directory(current/"covercache"/(retrykey+".jpg.source.json")),"blocked destination never replaced");
 fs::remove(current/"covercache"/(retrykey+".jpg.source.json"));migrate_profile_artwork(current.string());check(read(current/"covercache"/(retrykey+".jpg.source.json"))==read(old/"covercache"/(retrykey+".jpg.source.json")),"destination failure retry recovers exact pair");
 // Same-version previous installs are valid candidates; arbitrary prefix matches are not.
 const auto prev=base/"RLPlayer-3.3.1-win64.previous-0123456789abcdef0123456789abcdef";
 typed(prev,"6666666666666666",img);typed(badname,"7777777777777777",img);migrate_profile_artwork(current.string());
 check(fs::exists(current/"covercache/6666666666666666.jpg.source.json")&&!fs::exists(current/"covercache/7777777777777777.jpg.source.json"),"strict previous version candidate and unrelated-name exclusion");
 const auto historySource=base/"RLPlayer-3.1.0-win64";fs::create_directories(historySource);
 const std::string album="Release",artist="Private Artist",file="C:\\Private\\Release\\one.flac",albumkey=fnv(album+"\x1f"+"c:\\private\\release"),legacy=fnv(artist+"\x1f"+album);
 HistoryAlbumBinding binding{albumkey,legacy,album,artist,"tagged"};index(historySource,file,album,artist);write(historySource/"covercache"/(albumkey+".jpg"),img);
 std::vector<unsigned char> bytes;std::string mime;HistoryInfo info;
 auto history=import_legacy_history(current.string(),{binding});
 check(read_history(current.string(),binding,bytes,mime,&info)&&std::string(bytes.begin(),bytes.end())==img,"history import serves exact bound image");
 check(info.sourceVersion=="3.1.0"&&info.sourceKey==albumkey&&info.sha256==hash(img)&&mime=="image/png","history preserves version old key SHA and real MIME");
 std::string histj=read(current/"history-artwork"/(albumkey+".json"));
 check(histj.find("\"source\":\"history\"")!=std::string::npos&&histj.find("matchUrl")==std::string::npos&&histj.find("\"manual\"")==std::string::npos,"history never impersonates online or manual evidence");
 check(probe_history(current.string(),binding),"history probe agrees with complete image read");
 auto idempotent=import_legacy_history(current.string(),{binding});check(idempotent.historyRecords==0,"history import idempotent");
 HistoryAlbumBinding mismatch=binding;mismatch.artist="Other";check(!probe_history(current.string(),mismatch),"history artist mismatch fails closed");
 mismatch=binding;mismatch.album="Other";check(!probe_history(current.string(),mismatch),"history album mismatch fails closed");
 mismatch=binding;mismatch.identitySource="directory_inferred";check(!probe_history(current.string(),mismatch),"history inferred group cannot be promoted");
 write(current/"covercache"/(albumkey+".jpg.source.json"),"{\"source\":\"reset\"}");
 check(!probe_history(current.string(),binding),"current explicit declaration suppresses history");
 fs::remove(current/"covercache"/(albumkey+".jpg.source.json"));
 auto histblob=current/"history-artwork"/(albumkey+"."+hash(img)+".blob");std::string damaged=img;damaged.back()^=1;write(histblob,damaged);
 check(!probe_history(current.string(),binding)&&!read_history(current.string(),binding,bytes,mime),"history corruption rejected by probe and read");
 write(histblob,img);
 // Traversal in source declarations and metadata cover references is not a migration instruction.
 auto traversal=typed(old,"8888888888888888",img);auto pos=traversal.find("\"blob\":\"");auto end=traversal.find('"',pos+8);traversal.replace(pos+8,end-(pos+8),"../outside.blob");write(old/"covercache/8888888888888888.jpg.source.json",traversal);
 migrate_profile_artwork(current.string());check(!fs::exists(current/"covercache/8888888888888888.jpg.source.json"),"source blob traversal rejected");
 // Reparse points/junctions must not be walked, on source or destination.
 const auto outside=base/"outside";typed(outside,"9999999999999999",img);
 const auto linked=base/"RLPlayer-3.2.1-win64";std::error_code ec;fs::create_directory_symlink(outside,linked,ec);
 check(!ec,"test environment can create directory link");
 migrate_profile_artwork(current.string());check(!fs::exists(current/"covercache/9999999999999999.jpg.source.json"),"reparse source profile is rejected");
 const std::string newerMeta="{\"path\":\"C:\\\\Private\\\\one.flac\",\"fields\":{\"title\":{\"value\":\"CURRENT\",\"source\":\"user\"}}}";
 write(current/"rlplayer-meta/tracks"/(rawhash+".json"),newerMeta);migrate_profile_artwork(current.string());
 check(read(current/"rlplayer-meta/tracks"/(rawhash+".json"))==newerMeta,"current explicit Meta JSON always wins");
 // Real sharing denial: neither a missing nor a locked source is a committed pair.
 const std::string lockkey="aaaaaaaaaaaaaaaa";typed(old,lockkey,img);
 auto lockedBlob=old/"covercache"/(lockkey+"."+hash(img)+".blob");
 HANDLE lock=CreateFileW(lockedBlob.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
 check(lock!=INVALID_HANDLE_VALUE,"acquire real exclusive source file lock");
 migrate_profile_artwork(current.string());check(!fs::exists(current/"covercache"/(lockkey+".jpg.source.json")),"locked source cannot publish incomplete typed record");
 if(lock!=INVALID_HANDLE_VALUE)CloseHandle(lock);
 migrate_profile_artwork(current.string());check(read(current/"covercache"/(lockkey+".jpg.source.json"))==read(old/"covercache"/(lockkey+".jpg.source.json")),"unlock and retry completes original typed record");
 const std::string malformedMetaPath="C:\\Private\\bad.flac",badHash=fnv(malformedMetaPath);
 write(old/"rlplayer-meta/tracks"/(badHash+".json"),"{\"path\":\"C:\\\\Private\\\\bad.flac\",\"cover\":\"../outside.jpg\",\"fields\":{}}");
 migrate_profile_artwork(current.string());check(!fs::exists(current/"rlplayer-meta/tracks"/(badHash+".json")),"metadata cover traversal cannot publish JSON");
 const std::string mismatchHash=fnv("C:\\Other.flac");write(old/"rlplayer-meta/tracks"/(mismatchHash+".json"),metadata);
 migrate_profile_artwork(current.string());check(!fs::exists(current/"rlplayer-meta/tracks"/(mismatchHash+".json")),"metadata filename must bind exact stored path");
 const std::string partialPath="C:\\Private\\partial.flac",partialHash=fnv(partialPath);
 write(old/"rlplayer-meta/tracks"/(partialHash+".json"),"{\"path\":\"C:\\\\Private\\\\partial.flac\",\"cover\":\"covers/"+partialHash+".jpg\",\"fields\":{}}");
 migrate_profile_artwork(current.string());check(!fs::exists(current/"rlplayer-meta/tracks"/(partialHash+".json")),"declared missing Meta image cannot publish JSON");
 write(old/"rlplayer-meta/covers"/(partialHash+".jpg"),img);migrate_profile_artwork(current.string());
 check(fs::exists(current/"rlplayer-meta/tracks"/(partialHash+".json")),"Meta missing-image retry completes pair");
 // A legacy artist key is allowed only when its old index tuple is exact.
 const std::string oldAlbum="Older Album",oldArtist="Older Artist",oldPath="C:\\Private\\Older\\song.flac";
 const std::string oldKey=fnv(oldAlbum+"\x1f"+"c:\\private\\older"),oldLegacy=fnv(oldArtist+"\x1f"+oldAlbum);
 const auto v201=base/"RLPlayer-2.0.1-win64";index(v201,oldPath,oldAlbum,oldArtist);write(v201/"covercache"/(oldLegacy+".jpg"),img);
 HistoryAlbumBinding older{oldKey,oldLegacy,oldAlbum,oldArtist,"user_assigned"};
 import_legacy_history(current.string(),{older});
 check(read_history(current.string(),older,bytes,mime,&info)&&info.sourceVersion=="2.0.1"&&info.sourceKey==oldLegacy,"2.0.1 legacy artist-key cache binds current exact tuple");
 write(current/"covercache"/(oldKey+".jpg"),img);check(!probe_history(current.string(),older),"current untyped JPEG remains authoritative over history");
 fs::remove(current/"covercache"/(oldKey+".jpg"));
 const auto historyDeclaration=current/"history-artwork"/(oldKey+".json");auto savedHistory=read(historyDeclaration);
 write(historyDeclaration,"{\"version\":1,\"source\":\"history\",\"blob\":\"../outside\"}");
 check(!probe_history(current.string(),older),"malformed or traversal history declaration fails closed");write(historyDeclaration,savedHistory);
 const auto currentDev=base/"development-build";fs::create_directories(currentDev);
 migrate_profile_artwork(currentDev.string());check(!fs::exists(currentDev/"covercache"),"unnamed directories without explicit program version do not inherit profiles");
 const auto future=base/"RLPlayer-9.0.0-win64";typed(future,"bbbbbbbbbbbbbbbb",img);
 migrate_profile_artwork(current.string(),"3.3.1");check(!fs::exists(current/"covercache/bbbbbbbbbbbbbbbb.jpg.source.json"),"program version bounds future sibling candidates");
 // Warm probe must retain full byte validation on first access and invalidate
 // its proof on size/write/creation changes; image reads always hash all bytes.
 check(probe_history(current.string(),older)&&probe_history(current.string(),older),"stable history probes agree after proof caching");
 check(read_history(current.string(),older,bytes,mime)&&std::string(bytes.begin(),bytes.end())==img,"cached probes do not weaken subsequent image byte verification");
 const std::string resetTrack="C:\\Private\\deleted.flac",resetTrackRaw=fnv(resetTrack),resetTrackKey=fnv("c:\\private\\deleted.flac");
 const std::string resetIntent="{\"version\":1,\"key\":\"track:"+resetTrackKey+"\",\"source\":\"reset\"}";
 typed(old,"track:"+resetTrackKey,img);write(newer/"trackcovercache"/(resetTrackKey+".jpg.source.json"),resetIntent);
 write(old/"rlplayer-meta/tracks"/(resetTrackRaw+".json"),"{\"path\":\"C:\\\\Private\\\\deleted.flac\",\"cover\":\"covers/"+resetTrackRaw+".jpg\",\"fields\":{}}");
 write(old/"rlplayer-meta/covers"/(resetTrackRaw+".jpg"),img);
 migrate_profile_artwork(current.string());
 check(read(current/"trackcovercache"/(resetTrackKey+".jpg.source.json"))==resetIntent,"nearest profile reset intent migrates before older manual");
 check(!fs::exists(current/"rlplayer-meta/covers"/(resetTrackRaw+".jpg")),"migrated nearest reset blocks older Meta cover resurrection");
 const std::string badNear="dddddddddddddddd";typed(old,badNear,img);write(newer/"covercache"/(badNear+".jpg.source.json"),"{broken");
 migrate_profile_artwork(current.string());check(!fs::exists(current/"covercache"/(badNear+".jpg.source.json")),"bad nearer declaration blocks older valid manual fallback");
 const std::string repaired=typed(newer,badNear,alternate);migrate_profile_artwork(current.string());
 check(read(current/"covercache"/(badNear+".jpg.source.json"))==repaired,"nearer declaration repaired then retry succeeds without older overwrite");
 const std::string resetAlbum="Reset History",resetArtist="Reset Artist",resetFile="C:\\Private\\ResetHistory\\song.flac";
 HistoryAlbumBinding resetBinding{fnv(resetAlbum+"\x1f"+"c:\\private\\resethistory"),fnv(resetArtist+"\x1f"+resetAlbum),resetAlbum,resetArtist,"tagged"};
 const auto farProfile=base/"RLPlayer-2.0.0-win64";index(farProfile,resetFile,resetAlbum,resetArtist);write(farProfile/"covercache"/(resetBinding.key+".jpg"),img);
 write(newer/"covercache"/(resetBinding.key+".jpg.source.json"),"{\"version\":1,\"key\":\""+resetBinding.key+"\",\"source\":\"reset\"}");
 migrate_profile_artwork(current.string());import_legacy_history(current.string(),{resetBinding});
 check(!probe_history(current.string(),resetBinding)&&!fs::exists(current/"history-artwork"/(resetBinding.key+".json")),"nearest album reset prevents older untyped history revival");
 const std::string metaNearPath="C:\\Private\\meta-near.flac",metaNearHash=fnv(metaNearPath);
 const std::string metaNearGood="{\"path\":\"C:\\\\Private\\\\meta-near.flac\",\"fields\":{}}";
 write(old/"rlplayer-meta/tracks"/(metaNearHash+".json"),metaNearGood);write(newer/"rlplayer-meta/tracks"/(metaNearHash+".json"),"{broken");
 migrate_profile_artwork(current.string());check(!fs::exists(current/"rlplayer-meta/tracks"/(metaNearHash+".json")),"nearer malformed metadata is not silently replaced by older record");
 write(newer/"rlplayer-meta/tracks"/(metaNearHash+".json"),metaNearGood);migrate_profile_artwork(current.string());
 check(read(current/"rlplayer-meta/tracks"/(metaNearHash+".json"))==metaNearGood,"nearer metadata repair is retried without a global done marker");
 const auto linkedTarget=base/"RLPlayer-3.3.2-win64";fs::create_directories(linkedTarget);ec.clear();fs::create_directory_symlink(outside/"covercache",linkedTarget/"covercache",ec);
 check(!ec,"test environment creates destination link");migrate_profile_artwork(linkedTarget.string());
 check(!fs::exists(outside/"covercache"/(key+".jpg.source.json")),"destination reparse cannot write artwork outside target profile");
 const auto historyTarget=base/"RLPlayer-3.3.3-win64",historyOutside=base/"history-outside";fs::create_directories(historyTarget);fs::create_directories(historyOutside);
 ec.clear();fs::create_directory_symlink(historyOutside,historyTarget/"history-artwork",ec);check(!ec,"test environment creates history destination link");
 auto historyDenied=import_legacy_history(historyTarget.string(),{older});
 check(historyDenied.failed>0&&!fs::exists(historyOutside/(oldKey+".json")),"history publication rejects destination reparse and records retryable failure");
 auto changedMime=savedHistory;auto mimeAt=changedMime.find("image/png");changedMime.replace(mimeAt,9,"image/jpeg");write(historyDeclaration,changedMime);
 check(!probe_history(current.string(),older)&&!read_history(current.string(),older,bytes,mime),"cached history proof remains bound to declared MIME");write(historyDeclaration,savedHistory);
 auto changedSource=savedHistory;auto sourceAt=changedSource.find("\"sourceKey\":\"")+13;changedSource.replace(sourceAt,16,"0000000000000000");write(historyDeclaration,changedSource);
 check(!probe_history(current.string(),older),"history source key must be current or exact artist legacy key");write(historyDeclaration,savedHistory);
 auto fraction=savedHistory;auto lengthAt=fraction.find("\"bytes\":")+8;auto lengthEnd=fraction.find(',',lengthAt);fraction.insert(lengthEnd,".5");write(historyDeclaration,fraction);
 check(!probe_history(current.string(),older),"fractional history byte declaration is invalid");write(historyDeclaration,savedHistory);
 // Historical collection art is not a release-album claim. It is authorized
 // only for the identical old browsing group and exact physical/CUE members.
 const std::string groupAlbum="Private Group",groupFile="C:\\Private\\Group\\one.flac";
 const std::string groupKey=fnv(groupAlbum+"\x1f"+"c:\\private\\group");
 std::vector<HistoryMember> groupMembers{{groupFile,"",0,0,0}};
 HistoryAlbumBinding groupBinding{groupKey,fnv("\x1f"+groupAlbum),groupAlbum,"","directory_inferred",history_members_sha256(groupMembers),1};
 const auto groupSource=base/"RLPlayer-1.9.0-win64";index(groupSource,groupFile,groupAlbum,"");write(groupSource/"covercache"/(groupKey+".jpg"),img);
 import_legacy_history(current.string(),{groupBinding});
 check(read_history(current.string(),groupBinding,bytes,mime,&info)&&info.source=="historical_group"&&std::string(bytes.begin(),bytes.end())==img,"exact historical inferred group restores bytes without album promotion");
 auto groupRecord=read(current/"history-artwork"/(groupKey+".json"));
 check(groupRecord.find("\"kind\":\"historical_group\"")!=std::string::npos&&groupRecord.find(groupBinding.membersSha256)!=std::string::npos,"historical group provenance records kind and member fingerprint");
 auto otherGroup=groupBinding;otherGroup.memberCount=2;check(!probe_history(current.string(),otherGroup),"historical group member count change fails closed");
 otherGroup=groupBinding;auto changedMembers=groupMembers;changedMembers[0].path="C:\\Private\\Group\\other.flac";otherGroup.membersSha256=history_members_sha256(changedMembers);
 check(!probe_history(current.string(),otherGroup),"historical group path change fails closed");
 changedMembers=groupMembers;changedMembers[0].cuePath="disc.cue";changedMembers[0].cueNo=1;changedMembers[0].cueOff=75;
 otherGroup=groupBinding;otherGroup.membersSha256=history_members_sha256(changedMembers);check(!probe_history(current.string(),otherGroup),"historical group CUE membership change fails closed");
 otherGroup=groupBinding;otherGroup.membersSha256.clear();check(!probe_history(current.string(),otherGroup),"inferred group without exact member proof remains unavailable");
 auto reordered=std::vector<HistoryMember>{{"C:\\b.flac","",0,0,0},{"C:\\a.flac","",0,0,0}};auto sorted=reordered;std::reverse(sorted.begin(),sorted.end());
 check(history_members_sha256(reordered)==history_members_sha256(sorted),"member fingerprint ignores enumeration order");
 reordered.push_back(reordered.back());check(history_members_sha256(reordered)!=history_members_sha256(sorted),"member fingerprint retains duplicate segment cardinality");
 // An old source's explicit metadata must be applied before matching. This
 // mirrors Library.load and prevents old cache ownership from using stale tags.
 const auto assignedSource=base/"RLPlayer-1.8.0-win64";const std::string assignedFile="C:\\Private\\Assigned\\one.flac",assignedAlbum="User Release",assignedArtist="User Artist";
 index(assignedSource,assignedFile,"Stale raw tags","Wrong Artist");
 const std::string assignedKey=fnv(assignedAlbum+"\x1f"+"c:\\private\\assigned");
 write(assignedSource/"rlplayer-meta/tracks"/(fnv(assignedFile)+".json"),"{\"path\":\"C:\\\\Private\\\\Assigned\\\\one.flac\",\"fields\":{\"album\":{\"value\":\"User Release\",\"source\":\"user\"},\"albumArtist\":{\"value\":\"User Artist\",\"source\":\"user\"}}}");
 write(assignedSource/"covercache"/(assignedKey+".jpg"),img);
 HistoryAlbumBinding assignedBinding{assignedKey,fnv(assignedArtist+"\x1f"+assignedAlbum),assignedAlbum,assignedArtist,"user_assigned"};
 import_legacy_history(current.string(),{assignedBinding});check(read_history(current.string(),assignedBinding,bytes,mime),"source index applies its own explicit user Meta before ownership matching");
 const auto renamed=base/fs::u8path("播放器");fs::create_directories(renamed);
 migrate_profile_artwork(rta::net::artwork_migration::detail::utf8(renamed),"3.3.1");
 check(read(renamed/"covercache"/(key+".jpg.source.json"))==jpgmanifest,"explicit stable program version supports renamed Unicode current directory");
 const auto groupDeclaration=current/"history-artwork"/(groupKey+".json");const auto groupIndex=groupSource/"rlplayer-index.tsv";auto originalGroupIndex=read(groupIndex);
 fs::remove(groupDeclaration);write(groupIndex,originalGroupIndex+"C:\\Private\\Group\\two.flac\t0\t1\tPrivate Group\t\t\tOther\t2\t1\t2026\t44100\t16\t2\t10\t0\t\t\t0\t0\t0\n");
 import_legacy_history(current.string(),{groupBinding});check(!fs::exists(groupDeclaration),"nonempty subset of a larger old group is not sufficient for history import");
 write(groupIndex,originalGroupIndex);import_legacy_history(current.string(),{groupBinding});
 check(read_history(current.string(),groupBinding,bytes,mime,&info)&&info.source=="historical_group","exact group restored after rejected subset attempt remains retryable");
 const auto sourceMetaFile=assignedSource/"rlplayer-meta/tracks"/(fnv(assignedFile)+".json");
 const auto expectedMetaProof=hash(fnv(assignedFile)+".json:"+hash(read(sourceMetaFile))+"\n");
 check(read(current/"history-artwork"/(assignedKey+".json")).find(expectedMetaProof)!=std::string::npos,"history records exact source metadata digest alongside source index digest");
 printf("RESULT %d pass %d fail\n",passed,failed);return failed?1:0;
}

