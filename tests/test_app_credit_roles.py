"""Actual entity-builder credit role regression; private compiler fixture, no HTTP/profile/media."""
import pathlib, subprocess, tempfile, sys
root=pathlib.Path(__file__).resolve().parents[1]
code=r'''
#include "meta/entities.h"
#include <cstdio>
int main(){using namespace rta::meta;int n=0,f=0;auto ck=[&](bool v,const char*s){++n;f+=!v;printf("%s %s\n",v?"PASS":"FAIL",s);};
auto build=[](const std::string&a){return entity_batch_build_graph({entity_seed_from_tags("D:/public.wav",{{"title","Public"},{"album","Public album"},{"artist",a}})},123);};
auto named=[](const EntityGraph&g,const std::string&name){int n=0;for(auto&kv:g.entities)if((kv.second.kind==EntityKind::Person||kv.second.kind==EntityKind::Group)&&kv.second.name==name)++n;return n;};
auto edge=[](const EntityGraph&g,const std::string&name,const std::string&role){for(auto&kv:g.entities)for(auto&e:kv.second.edges){auto it=g.entities.find(e.to);if(e.role==role&&it!=g.entities.end()&&it->second.name==name)return true;}return false;};
const std::string raw="曲：A / 編曲：B / 歌：C / 演奏：D";auto g=build(raw);
ck(!named(g,raw),"explicit role string is not one artist");
ck(edge(g,"A","composer"),"Chinese composer credit");ck(edge(g,"B","arranger"),"traditional arranger credit");ck(edge(g,"C","vocalist"),"Japanese vocal credit");ck(edge(g,"D","performer"),"performer credit");
bool preserved=false;for(auto&kv:g.entities){auto it=kv.second.attrs.find("artistCredit");if(it!=kv.second.attrs.end()&&it->second.value==raw)preserved=true;}ck(preserved,"original credit preserved verbatim");
ck(g.count(EntityKind::Work)==0,"popular credit composer does not force classical work");
for(const char*name:{"AC/DC","Simon / Garfunkel","Unknown: A / B","A/B/C"})ck(named(build(name),name)==1,"natural slash or unknown prefix remains whole");
auto slash=build("Vocal: AC/DC / Composer: A");ck(edge(slash,"AC/DC","vocalist"),"slash within credited name preserved");
auto dup=build("Composer: A / Composer: A / Vocal: A");ck(named(dup,"A")==1&&edge(dup,"A","composer")&&edge(dup,"A","vocalist"),"duplicate person has distinct role edges without duplicate entity");
auto empty=build("Composer: / Vocal: C / Arranger:");ck(edge(empty,"C","vocalist")&&empty.count(EntityKind::Person)==1,"empty roles create no phantom person");
for(const auto&sample:std::vector<std::pair<std::string,std::string>>{{"作曲：A / 编曲：B","A"},{"作曲：A / 編曲：B","A"},{"Composer: A / Arranger: B","A"},{"作曲: A / 編曲: B","A"},{"Komponist: A / Arrangeur: B","A"},{"Compositeur: A / Arrangeur: B","A"},{"Compositor: A / Arreglista: B","A"}})ck(edge(build(sample.first),sample.second,"composer"),"explicit multilingual composer role");
auto odd=build("Composer: A/B / Vocal: C");ck(edge(odd,"A/B","composer"),"unmarked slash in value not split");
printf("checks=%d failures=%d\n",n,f);return f?1:0;}
'''
with tempfile.TemporaryDirectory(prefix='rlplayer-credit-roles-') as d:
 p=pathlib.Path(d);(p/'test.cpp').write_text(code,encoding='utf-8')
 subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe','-std=c++20','-O1','-I'+str(root/'src'),str(p/'test.cpp'),'-o',str(p/'test.exe'),'-lwinhttp','-lws2_32'],check=True)
 sys.exit(subprocess.run([str(p/'test.exe')],cwd=d).returncode)
