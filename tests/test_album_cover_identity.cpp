#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "net/library.h"
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <chrono>
#include <stdexcept>
namespace fs=std::filesystem;
static int failures=0,checks=0;
static void ck(bool v,const char* s){++checks;std::printf("%s %s\n",v?"PASS":"FAIL",s);if(!v)++failures;}
static std::string utf(const fs::path&p){auto s=p.u8string();return std::string(s.begin(),s.end());}
// Index fixtures address semantic columns by the emitted schema, never by the
// last field: v6 preserves albumSource and now appends codec after it.
static std::vector<std::string> index_fields(const std::string& line){std::vector<std::string> out;size_t start=0;for(size_t i=0;i<=line.size();++i)if(i==line.size()||line[i]=='\t'){out.push_back(line.substr(start,i-start));start=i+1;}return out;}
static std::string index_join(const std::vector<std::string>& fields){std::string line;for(size_t i=0;i<fields.size();++i){if(i)line+='\t';line+=fields[i];}return line;}
static size_t index_column(const std::string& text,const char* name){auto begin=text.find("#path\t");if(begin==std::string::npos)throw std::runtime_error("private index fixture missing schema");auto end=text.find('\n',begin);auto fields=index_fields(text.substr(begin,end==std::string::npos?end:end-begin));auto it=std::find(fields.begin(),fields.end(),name);if(it==fields.end())throw std::runtime_error(std::string("private index fixture missing column: ")+name);return size_t(it-fields.begin());}
static void fixture(const fs::path& root, const fs::path& ffmpeg) {
 fs::create_directories(root);std::ofstream(root/"fixture-marker.txt")<<"private generated fixture";
 auto put16=[](std::ofstream& f,unsigned v){f.put(char(v));f.put(char(v>>8));};
 auto put32=[](std::ofstream& f,unsigned v){for(int i=0;i<4;++i)f.put(char(v>>(8*i)));};
 auto wav=root/"zero.wav";{std::ofstream f(wav,std::ios::binary);f<<"RIFF";put32(f,3236);f<<"WAVEfmt ";put32(f,16);put16(f,1);put16(f,2);put32(f,8000);put32(f,32000);put16(f,4);put16(f,16);f<<"data";put32(f,3200);std::string zero(3200,'\0');f.write(zero.data(),zero.size());}
 auto quote=[](const fs::path& p){return L"\""+p.wstring()+L"\"";};
 auto run=[&](const std::wstring& args){if(rta::audio::run_hidden(quote(ffmpeg)+L" -v error -y "+args,nullptr)!=0)throw std::runtime_error("private fixture FFmpeg failed");};
 for(unsigned seed:{1u,77u}){
  auto bmp=root/(std::to_string(seed)+".bmp");{std::ofstream f(bmp,std::ios::binary);f<<"BM";put32(f,54+64*192);put32(f,0);put32(f,54);put32(f,40);put32(f,64);put32(f,64);put16(f,1);put16(f,24);put32(f,0);put32(f,64*192);for(int i=0;i<4;++i)put32(f,0);unsigned x=seed;for(int i=0;i<64*192;++i){x^=x<<13;x^=x>>17;x^=x<<5;f.put(char(x));}}
  run(L"-i "+quote(bmp)+L" -frames:v 1 "+quote(root/(std::to_string(seed)+".png")));
 }
 struct Item{const char* dir;const char* file;const char* album;unsigned picture;};
 Item items[]={{"Mixed","unknown.wav","",0},{"Mixed","tagged.flac","Real Album",0},{"OwnVsFolder","own.flac","Own Album",1},{"Collection","with-picture.flac","",1},{"Collection","without-picture.wav","",0},{"Trusted","with-picture.flac","Trusted Album",1},{"Trusted","without-picture.flac","Trusted Album",0},{"OwnVariants","first.flac","Variants Album",1},{"OwnVariants","second.flac","Variants Album",77}};
 for(auto i:items){auto dst=root/"music"/i.dir/i.file;fs::create_directories(dst.parent_path());std::wstring args=L"-i "+quote(wav);if(i.picture)args+=L" -i "+quote(root/(std::to_string(i.picture)+".png"))+L" -map 0:a -map 1:v -c:v copy -disposition:v attached_pic";if(*i.album)args+=L" -metadata album=\""+rta::audio::widen(i.album)+L"\" -metadata artist=\"Fixture Artist\"";if(dst.extension()==L".flac")args+=L" -c:a flac";run(args+L" -metadata title=\""+rta::audio::widen(i.file)+L"\" "+quote(dst));}
 fs::copy_file(root/"77.png",root/"music/OwnVsFolder/cover.png",fs::copy_options::overwrite_existing);
}
int main(int argc,char**argv){
 if(argc<2||argc>3){std::fprintf(stderr,"usage: test_album_cover_identity <fixture-ffmpeg> [absolute-private-output-dir]\n");return 2;}
 fs::path full=fs::absolute(fs::u8path(argv[1]));if(!fs::exists(full)&&std::string(argv[1])=="ffmpeg.exe")full=fs::u8path(rta::audio::find_tool("ffmpeg"));if(!fs::exists(full))return 2;
 fs::path root=argc==3?fs::u8path(argv[2]):fs::temp_directory_path()/("rlplayer-album-identity-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));if(!root.is_absolute())return 2;
 try{fixture(root,full);}catch(const std::exception& e){std::fprintf(stderr,"FAIL fixture: %s\n",e.what());return 2;}
 std::printf("private_fixture=%s\n",utf(root).c_str());
 const auto profile=root/"profile";fs::create_directories(profile);_putenv_s("RLPLAYER_LIBRARY_DIR",utf(profile).c_str());_putenv_s("RLPLAYER_PORT","");
 {std::ofstream conf(profile/"rlplayer-library.conf");conf<<utf(root/"music")<<"\n";} auto&lib=rta::net::Library::instance();lib.load();lib.addRootEx(utf(root/"music"));lib.startScan(true);lib.joinScan();
 ck(lib.trackCount()==9,"all nine private media fixtures scanned");
 auto track=[&](const char*folder,const char*name){rta::net::LibTrack t;auto p=utf(root/"music"/folder/name);for(size_t i=0;i<lib.trackCount();++i){rta::net::LibTrack x;if(lib.trackAt(i,x)&&x.path==p)return x;}return t;};
 auto unknown=track("Mixed","unknown.wav");ck(!unknown.path.empty()&&unknown.albumFromDir,"missing ALBUM remains explicitly directory-inferred in fresh scan");
 auto albums=lib.albums();std::string unknownKey;for(const auto&a:albums){if(std::find_if(a.idx.begin(),a.idx.end(),[&](size_t i){rta::net::LibTrack t;return lib.trackAt(i,t)&&t.path==unknown.path;})!=a.idx.end())unknownKey=a.key;}
 ck(!unknownKey.empty(),"untagged track remains represented in library grouping");
 rta::net::LibCoverTerm term;ck(!lib.albumCoverTerm(unknownKey,term),"directory-only identity cannot become an online album query");
 rta::net::LibApiCoverTerm titleTerm;ck(!lib.albumCoverTermTitles(unknownKey,3,titleTerm),"directory-only identity cannot become title-assisted online album query");
 auto terms=lib.albumsWithoutCover();ck(std::none_of(terms.begin(),terms.end(),[&](const auto&t){return t.key==unknownKey;}),"bulk cover terms omit inferred identity");
 auto titles=lib.albumsWithoutCoverTitles();ck(std::none_of(titles.begin(),titles.end(),[&](const auto&t){return t.key==unknownKey;}),"bulk title terms omit inferred identity");
 std::string mime;auto ownPath=utf(root/"music"/"OwnVsFolder"/"own.flac");auto own=rta::net::lib_cover_embedded_bytes(ownPath,mime);ck(own.size()>1024,"actual embedded image fixture is extractable");
 std::vector<unsigned char>b;ck(lib.trackCover(ownPath,b,mime)&&b==own,"track own embedded image wins over directory cover even in one album");
 auto groupOwn=utf(root/"music"/"Collection"/"with-picture.flac");auto groupMissing=utf(root/"music"/"Collection"/"without-picture.wav");
 auto pic=rta::net::lib_cover_embedded_bytes(groupOwn,mime);ck(pic.size()>1024,"inferred-group embedded image fixture is extractable");
 ck(lib.trackCover(groupOwn,b,mime)&&b==pic,"inferred-group track keeps its own embedded image");
 ck(lib.trackCover(groupMissing,b,mime)&&b.empty(),"inferred collection never borrows another track image");
 auto trustedOwn=utf(root/"music"/"Trusted"/"with-picture.flac");auto trustedMissing=utf(root/"music"/"Trusted"/"without-picture.flac");auto trustedPic=rta::net::lib_cover_embedded_bytes(trustedOwn,mime);
 ck(trustedPic.size()>1024&&lib.trackCover(trustedMissing,b,mime)&&b==trustedPic,"trusted tagged album allows same-album embedded fallback");
 auto first=utf(root/"music"/"OwnVariants"/"first.flac");auto second=utf(root/"music"/"OwnVariants"/"second.flac");auto firstPic=rta::net::lib_cover_embedded_bytes(first,mime);auto secondPic=rta::net::lib_cover_embedded_bytes(second,mime); ck(firstPic.size()>1024&&secondPic.size()>1024&&firstPic!=secondPic,"same album has two genuinely different embedded images");ck(lib.trackCover(first,b,mime)&&b==firstPic,"first same-album track keeps own image");ck(lib.trackCover(second,b,mime)&&b==secondPic,"second same-album track keeps own image independent of representative"); lib.load();auto restored=track("Mixed","unknown.wav");ck(restored.albumFromDir,"directory provenance survives index save and reload");

 rta::net::LibAlbumIdentity ident;
 ck(lib.trackAlbumIdentity(restored.path,ident)&&!ident.onlineCoverEligible&&ident.source==rta::net::LibAlbumIdentitySource::DirectoryInferred,"canonical track identity explicitly marks directory group untrusted");
 ck(lib.albumIdentity(ident.key,ident)&&ident.name=="Mixed","album identity shares stable canonical grouping key");
 auto tagged=track("Mixed","tagged.flac");
 ck(lib.trackAlbumIdentity(tagged.path,ident)&&ident.onlineCoverEligible&&ident.source==rta::net::LibAlbumIdentitySource::Tagged,"actual tagged release remains online eligible");
 ck(std::string(rta::net::lib_album_identity_source_name(ident.source))=="tagged","identity source has stable API string");
 lib.startScan(false);lib.joinScan();restored=track("Mixed","unknown.wav");
 ck(restored.albumFromDir&&restored.albumIdentitySource==rta::net::LibAlbumIdentitySource::DirectoryInferred,"incremental scan retains inferred provenance");
 // Convert only our own generated v6 cache to its real legacy v5 representation.
 auto index=profile/"rlplayer-index.tsv";std::ifstream input(index,std::ios::binary);std::string text((std::istreambuf_iterator<char>(input)),{});input.close();
 const size_t legacyColumns=index_column(text,"albumSource");
 std::string legacy;size_t start=0;while(start<text.size()){
  auto end=text.find('\n',start);if(end==std::string::npos)end=text.size();auto line=text.substr(start,end-start);
  if(line.rfind("#libIndexV",0)==0)line="#libIndexV\t5";
  else if(line.rfind("#sourceDepthV",0)==0){start=end+1;continue;}
  else if(line.rfind("#path",0)==0||(!line.empty()&&line[0]!='#')){auto fields=index_fields(line);fields.resize(legacyColumns);line=index_join(fields);}
  legacy+=line+"\n";start=end+1;
 }
 {std::ofstream output(index,std::ios::binary|std::ios::trunc);output<<legacy;}
 lib.load();ck(lib.indexNeedsRetag(),"v5 cache automatically requests metadata migration at startup");
 auto legacyTrack=track("Mixed","tagged.flac");ck(lib.trackAlbumIdentity(legacyTrack.path,ident)&&!ident.onlineCoverEligible&&ident.source==rta::net::LibAlbumIdentitySource::LegacyUnknown,"v5 album text never silently becomes trusted tag evidence");
 lib.startScan(false);lib.joinScan();ck(!lib.indexNeedsRetag(),"successful automatic migration resolves provenance without manual full rescan");
 restored=track("Mixed","unknown.wav");tagged=track("Mixed","tagged.flac");
 ck(restored.albumFromDir&&tagged.albumIdentitySource==rta::net::LibAlbumIdentitySource::Tagged,"migration reprobes original metadata and separates inferred/tagged records");
 lib.load();ck(!lib.indexNeedsRetag()&&track("Mixed","unknown.wav").albumFromDir,"v6 migration is durable across reload");

 {std::ifstream f(index,std::ios::binary);text.assign(std::istreambuf_iterator<char>(f),{});}
 const size_t sourceColumn=index_column(text,"albumSource"),codecColumn=index_column(text,"codec");
 bool sourceFound=false,onlySourceChanged=false;std::string savedCodec,corruptedIndex;start=0;
 while(start<text.size()){
  auto end=text.find('\n',start);if(end==std::string::npos)end=text.size();auto line=text.substr(start,end-start);
  if(!line.empty()&&line[0]!='#'){
   auto fields=index_fields(line);
   if(fields.size()>std::max(sourceColumn,codecColumn)&&fields[0]==tagged.path&&fields[sourceColumn]=="tagged"){
    sourceFound=true;savedCodec=fields[codecColumn];const auto before=fields;fields[sourceColumn]="corrupt_source";
    onlySourceChanged=sourceColumn!=codecColumn&&fields[codecColumn]==before[codecColumn];
    for(size_t i=0;i<fields.size();++i)if(i!=sourceColumn&&fields[i]!=before[i])onlySourceChanged=false;
    line=index_join(fields);
   }
  }
  corruptedIndex+=line+"\n";start=end+1;
 }
 ck(sourceFound&&onlySourceChanged&&!savedCodec.empty(),"v6 named albumSource column is persisted separately from codec and only source is corrupted");
 if(sourceFound){std::ofstream f(index,std::ios::binary|std::ios::trunc);f<<corruptedIndex;}
 lib.load();rta::net::LibAlbumIdentity corruptedIdentity;lib.trackAlbumIdentity(tagged.path,corruptedIdentity);
 ck(lib.indexNeedsRetag()&&corruptedIdentity.source==rta::net::LibAlbumIdentitySource::LegacyUnknown&&!corruptedIdentity.onlineCoverEligible&&track("Mixed","tagged.flac").codec==savedCodec,"invalid v6 provenance remains untrusted and requests automatic repair while codec stays intact");
 lib.startScan(false);lib.joinScan();auto repaired=track("Mixed","tagged.flac");
 ck(!lib.indexNeedsRetag()&&repaired.albumIdentitySource==rta::net::LibAlbumIdentitySource::Tagged&&repaired.album=="Real Album"&&repaired.codec==savedCodec,"invalid named v6 source repairs from actual metadata in incremental scan without damaging codec");
 // User-confirmed identities are overlays, never edits to the generated media.
 auto& store=rta::meta::MetaStore::instance();store.setBaseDir(utf(profile/"meta"));
 rta::meta::TrackMeta confirmed;confirmed.path=restored.path;
 for(auto kv:std::vector<std::pair<std::string,std::string>>{{"album","Confirmed Release"},{"artist","Confirmed Singer"},{"albumArtist","Confirmed Album Artist"},{"year","2001"},{"trackNo","7"},{"discNo","2"}})confirmed.fields[kv.first]={kv.second,"user",1,1.f};
 store.save(confirmed);lib.load();auto corrected=track("Mixed","unknown.wav");
 ck(corrected.album=="Confirmed Release"&&corrected.artist=="Confirmed Singer"&&corrected.albumArtist=="Confirmed Album Artist"&&corrected.year==2001&&corrected.trackNo==7&&corrected.discNo==2,"locked user fields overlay the loaded real library");
 ck(corrected.albumIdentitySource==rta::net::LibAlbumIdentitySource::UserAssigned&&!corrected.albumFromDir,"confirmed album has explicit user-assigned provenance");
 lib.startScan(true);lib.joinScan();corrected=track("Mixed","unknown.wav");
 ck(corrected.album=="Confirmed Release"&&corrected.trackNo==7,"forced fresh probe reapplies confirmed fields without editing media");
 rta::net::LibAlbumIdentity untrusted;lib.trackAlbumIdentity(groupOwn,untrusted);rta::net::LibAlbum untrustedAlbum;lib.album(untrusted.key,untrustedAlbum);
 ck(untrustedAlbum.hasCover&&untrustedAlbum.coverSrc==rta::net::kCoverEmbedded&&!untrustedAlbum.onlineCoverEligible,"inferred browsing group keeps its own local image without becoming an online release");
 std::vector<rta::net::LibTrack> sparse(4);
 sparse[0].path=confirmed.path;sparse[0].cuePath="private.cue";sparse[0].cueNo=1;sparse[0].cueOff=123;sparse[0].cueLen=456;sparse[0].title="CUE One";sparse[0].artist="CUE Performer";sparse[0].trackNo=1;
 sparse[1]=sparse[0];sparse[1].cueNo=2;sparse[1].cueOff=579;sparse[1].title="CUE Two";sparse[1].trackNo=2;
 sparse[2].path="foreign://untouched";sparse[2].album="Foreign Album";sparse[2].albumIdentitySource=rta::net::LibAlbumIdentitySource::LegacyUnknown;
 sparse[3].path=tagged.path;sparse[3].album="Original Tag";sparse[3].artist="Original Artist";
 rta::meta::TrackMeta suggestion;suggestion.path=tagged.path;suggestion.fields["album"]={"Unconfirmed Guess","llm",2,1.f};store.save(suggestion);
 rta::net::lib_apply_user_metadata(sparse);
 ck(sparse[0].album=="Confirmed Release"&&sparse[1].album=="Confirmed Release"&&sparse[0].title=="CUE One"&&sparse[1].title=="CUE Two"&&sparse[0].artist=="CUE Performer"&&sparse[0].trackNo==1&&sparse[1].trackNo==2&&sparse[0].cueOff==123&&sparse[1].cueOff==579&&sparse[0].cueLen==456,"physical-file assignment preserves distinct CUE segment semantics");
 ck(sparse[2].path=="foreign://untouched"&&sparse[2].album=="Foreign Album"&&sparse[2].albumIdentitySource==rta::net::LibAlbumIdentitySource::LegacyUnknown,"sparse overlay preserves unfamiliar index sources and virtual entries");
 ck(sparse[3].album=="Original Tag"&&sparse[3].artist=="Original Artist","unconfirmed LLM metadata never overrides library release identity");
 lib.load();corrected=track("Mixed","unknown.wav");ck(corrected.album=="Confirmed Release"&&corrected.albumIdentitySource==rta::net::LibAlbumIdentitySource::UserAssigned,"user assignment survives scan serialization and library reload");
 lib.trackAlbumIdentity(trustedOwn,ident);terms=lib.albumsWithoutCover();titles=lib.albumsWithoutCoverTitles();
 ck(std::any_of(terms.begin(),terms.end(),[&](const auto&t){return t.key==ident.key;})&&std::any_of(titles.begin(),titles.end(),[&](const auto&t){return t.key==ident.key;}),"trusted local artwork does not block release artwork search candidates");
 // Foreign/deleted-library records may contain huge lyrics; none is ours.
 const auto metaTracks=profile/"meta"/"tracks";
 {std::ofstream f(metaTracks/(rta::meta::MetaStore::hashOf("removed-library.wav")+".json"),std::ios::binary);f<<"{\"path\":\"removed-library.wav\",\"fields\":{\"lyrics\":{\"source\":\"user\",\"value\":\"";const std::string chunk(1024*1024,'x');for(int n=0;n<32;++n)f.write(chunk.data(),chunk.size());f<<"\"}}}";}
 {std::ofstream f(metaTracks/fs::u8path("异常名😀.json"));f<<"not JSON";}
 {std::ofstream f(metaTracks/"not-a-valid-hash.json");f<<"not JSON";}
 const auto sparseBegin=std::chrono::steady_clock::now();bool safe=true;
 try{rta::net::lib_apply_user_metadata(sparse);}catch(...){safe=false;}
 const double sparseMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-sparseBegin).count();
 std::printf("sparse_overlay_foreign_lyrics_bytes=33554432 elapsed_ms=%.3f\n",sparseMs);
 ck(safe&&sparse[0].album=="Confirmed Release"&&sparse[2].album=="Foreign Album","32MiB foreign lyrics and Unicode/invalid hash filenames never affect active library overrides");
 // Real Duca metadata keeps n/total strings in MetaStore, while the library
 // uses the leading ordinal for disc/track grouping and sorting.
 confirmed.fields["trackNo"]={"1/11","user",3,1.f};confirmed.fields["discNo"]={"2/2","user",3,1.f};store.save(confirmed);
 std::vector<rta::net::LibTrack> ordinal(2);ordinal[0].path=confirmed.path;ordinal[0].trackNo=9;ordinal[0].discNo=1;ordinal[0].discFromDir=true;ordinal[1]=ordinal[0];ordinal[1].cuePath="ordinal.cue";ordinal[1].cueNo=3;ordinal[1].cueOff=999;
 rta::net::lib_apply_user_metadata(ordinal);
 ck(ordinal[0].trackNo==1&&ordinal[0].discNo==2&&!ordinal[0].discFromDir,"Duca 1/11 and 2/2 override initial disc 1 with library ordinals 1 and 2");
 ck(ordinal[1].trackNo==9&&ordinal[1].discNo==1&&ordinal[1].cueNo==3&&ordinal[1].cueOff==999,"fractional physical-file override preserves CUE segment ordinals and range");
 rta::meta::TrackMeta raw;store.load(confirmed.path,raw);ck(raw.fields["trackNo"].value=="1/11"&&raw.fields["discNo"].value=="2/2","library ordinal parsing never rewrites original metadata strings for API");
 lib.startScan(true);lib.joinScan();corrected=track("Mixed","unknown.wav");ck(corrected.trackNo==1&&corrected.discNo==2,"forced scan applies real Duca n/total ordering durably");
 for(const char* invalid:{"3/2","1/xx","1/2/3","2147483648","1/2147483648","0","0/2","1/0","-1"," 1","1/","/2"}){
  confirmed.fields["trackNo"]={invalid,"user",4,1.f};confirmed.fields["discNo"]={invalid,"user",4,1.f};confirmed.fields["year"]={"2015/2","user",4,1.f};store.save(confirmed);
  ordinal[0].trackNo=5;ordinal[0].discNo=6;ordinal[0].year=1999;ordinal[0].discFromDir=true;rta::net::lib_apply_user_metadata(ordinal);
  ck(ordinal[0].trackNo==5&&ordinal[0].discNo==6&&ordinal[0].year==1999&&ordinal[0].discFromDir,"invalid ordinal and fractional year do not contaminate existing values or provenance");
 }
 confirmed.fields["trackNo"]={"2147483647","user",5,1.f};confirmed.fields["discNo"]={"2","user",5,1.f};confirmed.fields["year"]={"2015","user",5,1.f};store.save(confirmed);rta::net::lib_apply_user_metadata(ordinal);
 ck(ordinal[0].trackNo==2147483647&&ordinal[0].discNo==2&&ordinal[0].year==2015,"bounded positive integer maximum and plain numeric year remain supported");
 // A confirmed group and its individual tracks have different provenance.
 const auto mixedProfile=root/"mixed-profile";fs::create_directories(mixedProfile);_putenv_s("RLPLAYER_LIBRARY_DIR",utf(mixedProfile).c_str());
 auto mixedIndex=[&](bool reverse,const char* extra){
  std::ofstream f(mixedProfile/"rlplayer-index.tsv",std::ios::binary);f<<"#libIndexV\t6\n";
  auto row=[&](const std::string& p,const char* source){f<<p<<"\t1\t1\tMixed Confirmed\tFixture Artist\tFixture Artist\tTrack\t1\t1\t2015\t8000\t16\t2\t0.1\t0\t\t\t0\t0\t0\t"<<source<<"\n";};
  if(reverse){row(trustedMissing,"tagged");row(trustedOwn,"user_assigned");}else{row(trustedOwn,"user_assigned");row(trustedMissing,"tagged");}
  if(extra)row(utf(root/"music"/"Trusted"/"foreign.wav"),extra);
 };
 for(bool reverse:{false,true}){
  mixedIndex(reverse,nullptr);lib.load();rta::net::LibAlbumIdentity confirmedId,taggedId,groupId;
  lib.trackAlbumIdentity(trustedOwn,confirmedId);lib.trackAlbumIdentity(trustedMissing,taggedId);lib.albumIdentity(confirmedId.key,groupId);
  ck(groupId.source==rta::net::LibAlbumIdentitySource::UserAssigned&&groupId.onlineCoverEligible,"mixed group confirmation is independent of indexed row ordering");
  ck(confirmedId.source==rta::net::LibAlbumIdentitySource::UserAssigned&&taggedId.source==rta::net::LibAlbumIdentitySource::Tagged&&confirmedId.key==taggedId.key,"track identity keeps its actual source and does not borrow another track user confirmation");
  for(const char* source:{"legacy_unknown","directory_inferred"}){
   mixedIndex(reverse,source);lib.load();lib.trackAlbumIdentity(trustedOwn,confirmedId);lib.albumIdentity(confirmedId.key,groupId);
   std::printf("MIX extra=%s group=%s eligible=%d confirmed=%s tracks=%zu\n",source,rta::net::lib_album_identity_source_name(groupId.source),groupId.onlineCoverEligible,rta::net::lib_album_identity_source_name(confirmedId.source),lib.trackCount());
   ck(!groupId.onlineCoverEligible&&!rta::net::lib_album_identity_trusted(groupId.source)&&confirmedId.source==rta::net::LibAlbumIdentitySource::UserAssigned,"untrusted group member vetoes group eligibility while confirmed track keeps its own source");
  }
 }
 std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}



