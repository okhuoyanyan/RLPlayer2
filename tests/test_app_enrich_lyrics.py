"""Execute the production enrich source loop with offline provider callbacks.

This tests selection, not HTTP providers or a full filesystem enrichment run.
"""
import os, pathlib, subprocess, sys, tempfile
root = pathlib.Path(os.environ.get('RLPLAYER_TEST_SOURCE_ROOT', pathlib.Path(__file__).resolve().parents[1]))
text = (root/'src/meta/enrich.h').read_text(encoding='utf-8')
start = text.index('            for (const ProviderDesc* pd : art_lyric_sources(opts.srcOn))')
end = text.index('            if (lr.ok && !lr.lrc.empty())', start)
loop = text[start:end].replace('art_lyric_sources(opts.srcOn)', 'providers')
code = r'''
#include "meta/provider.h"
#include <cstdio>
using namespace rta::meta;
static std::vector<LyricResult> answers;
static int calls=0;
static LyricResult reply(const LyricRequest&,std::string&){return answers.at(calls++);}
static LyricResult run(){
 ProviderDesc a("a","a",pv_caps(ArtCap::Lyrics),nullptr,reply);
 std::vector<const ProviderDesc*> providers(answers.size(),&a);
 LyricRequest lrq; LyricResult lr; std::vector<std::string> lyrErrs;
''' + loop + r'''
 return lr;
}
int main(){int checks=0, failures=0;
 auto check=[&](bool ok,const char* label){++checks;if(!ok)++failures;printf("%s %s\n",ok?"PASS":"FAIL",label);};
 auto result=[](const std::string& text){LyricResult r;r.ok=true;r.lrc=text;r.source="fixture";return r;};
 const std::string real="[00:05.00]晴天里有风\n[00:09.50]吹过你的发梢\n[00:14.00]我在原地等一个回答";
 for(const auto& placeholder:{std::string("纯音乐，请欣赏"),std::string("[00:00.00]暂无歌词"),std::string("Instrumental"),std::string("")}){
  answers={result(placeholder),result(real)};calls=0;auto got=run();
  check(got.ok&&got.lrc==real&&calls==2,"placeholder or empty success continues to real later source");
  answers={result(placeholder)};calls=0;got=run();
  check(!got.ok&&got.lrc.empty(),"placeholder alone is never selected for persistence");
 }
 answers={result(real),result("Instrumental")};calls=0;auto got=run();
 check(got.lrc==real&&calls==1,"real short lyrics keep source priority");
 answers={LyricResult{},result(real)};calls=0;got=run();
 check(got.lrc==real&&calls==2,"failed source still recovers through later source");
 printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
with tempfile.TemporaryDirectory(prefix='rlplayer-enrich-lyrics-') as directory:
 path=pathlib.Path(directory);cpp=path/'test.cpp';exe=path/'test.exe';cpp.write_text(code,encoding='utf-8')
 subprocess.run([compiler,'-std=c++17','-O2','-I'+str(root/'src'),str(cpp),'-o',str(exe),'-lwinhttp','-lws2_32'],check=True)
 sys.exit(subprocess.run([str(exe)],cwd=directory).returncode)
