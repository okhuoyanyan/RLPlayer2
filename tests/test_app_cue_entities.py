"""CUE identities at the actual Library-to-entity boundary; no media/device IO."""
import pathlib, subprocess, sys, tempfile
root=pathlib.Path(__file__).resolve().parents[1]
code=r'''
#include "net/library.h"
#include <cstdio>
#include <fstream>
int main(int argc,char**argv){using namespace rta::net;using namespace rta::meta;
 int checks=0,failures=0;auto check=[&](bool ok,const char* label){++checks;if(!ok)++failures;printf("%s %s\n",ok?"PASS":"FAIL",label);};
 LibTrack a;a.path="D:/offline-fixture/Disc 1/whole.wav";a.album="Fixture";a.artist="Fixture Artist";
 a.title="First";a.cuePath="D:/offline-fixture/album.cue";a.cueNo=1;a.cueOff=0;a.cueLen=44100;a.trackNo=1;
 LibTrack b=a;b.title="Second";b.cueNo=2;b.cueOff=44100;b.trackNo=2;
 auto cue=entity_graph_from_library_tracks({a,b},123);
 check(cue.count(EntityKind::Track)==2,"two virtual tracks sharing physical file keep two Track entities");
 check(cue.count(EntityKind::Recording)==2,"two CUE titles keep separate Recording entities");
 auto reordered=entity_graph_from_library_tracks({b,a},123);
 check(cue.entities.size()==reordered.entities.size(),"input order does not change entity count");
 bool ids=true;for(const auto& kv:cue.entities)if(reordered.entities.find(kv.first)==reordered.entities.end())ids=false;
 check(ids,"input order does not change stable entity IDs");
 LibTrack c=a;c.cuePath.clear();c.cueNo=0;c.cueOff=0;c.cueLen=0;
 auto plain=entity_graph_from_library_tracks({c},123);
 auto seed=entity_seed_from_track_path(c.path,{{"title",c.title},{"artist",c.artist},{"album",c.album},{"track","1"}});
 auto expected=entity_batch_build_graph({seed},123);
 bool stable=true;for(const auto& kv:expected.entities)if(kv.second.kind==EntityKind::Track&&plain.entities.find(kv.first)==plain.entities.end())stable=false;
 check(stable&&plain.count(EntityKind::Track)==1,"ordinary file Track identity stays compatible");
 LibTrack d=b;d.path="D:/offline-fixture/Disc 2/whole.wav";
 check(entity_graph_from_library_tracks({a,b,d},123).count(EntityKind::Track)==3,"multi-FILE CUE identity stays distinct");
 // Real load/query from a private v5 index, with FILE names deliberately ordered
 // opposite to global track number. No scan or media decoder is invoked.
 const std::string home=argv[1];_putenv_s("RLPLAYER_LIBRARY_DIR",home.c_str());_putenv_s("RLPLAYER_PORT","");
 std::ofstream(home+"/rlplayer-library.conf")<<home<<"\n";
 auto writeIndex=[&](bool badPath){std::ofstream index(home+"/rlplayer-index.tsv");index<<"#libIndexV\t5\n";
  auto row=[&](const std::string& file,int n,const char* duration){index<<file<<"\t0\t0\tFixture\t\tFixture Artist\tCue "<<n<<"\t"<<n<<"\t1\t0\t44100\t16\t2\t"<<duration<<"\t0\t\t"<<home<<"/album.cue\t"<<n<<"\t"<<(n-1)*44100<<"\t44100\n";};
  row(home+"/a-second.wav",3,"1");row(home+"/z-first.wav",1,"nan");row(home+"/a-second.wav",2,"1");
  if(badPath)row(home+"/"+char(0xff)+"invalid.wav",4,"1");};
 writeIndex(true);auto& lib=Library::instance();lib.load();check(lib.trackCount()==3,"invalid UTF-8 cached path is excluded before filesystem use");
 writeIndex(false);lib.load();auto albums=lib.albums();check(albums.size()==1&&albums[0].idx.size()==3,"multi-FILE CUE rows load into one local album");
 bool ordered=albums.size()==1;int expectedNo=1;if(ordered)for(auto i:albums[0].idx){LibTrack t;lib.trackAt(i,t);ordered=ordered&&t.cueNo==expectedNo++;}
 check(ordered,"album rows follow global CUE track numbers across FILE boundaries");
 check(albums.size()==1&&std::isfinite(albums[0].duration)&&albums[0].duration==2,"nonfinite cached duration cannot poison album totals or JSON");
 printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
with tempfile.TemporaryDirectory(prefix='rlplayer-cue-entities-') as d:
 p=pathlib.Path(d);cpp=p/'test.cpp';exe=p/'test.exe';cpp.write_text(code,encoding='utf-8')
 subprocess.run([compiler,'-std=c++20','-O1','-I'+str(root/'src'),str(cpp),'-o',str(exe),'-lwinhttp','-lws2_32'],check=True)
 sys.exit(subprocess.run([str(exe),str(p)],cwd=d).returncode)
