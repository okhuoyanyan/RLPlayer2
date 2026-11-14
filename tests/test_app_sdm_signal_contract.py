"""Actual signal JSON writer fragments and effective-order normalization."""
import pathlib,subprocess,sys,tempfile
root=pathlib.Path(__file__).resolve().parents[1];source=(root/'tools/rtaweb.cpp').read_text(encoding='utf-8')
fields=['j'+line.strip() for line in source.splitlines() if '<< ",\\\"modOrderRequested\\\"' in line]
carrier=next('j'+line.strip() for line in source.splitlines() if '<< ",\\\"carrier\\\":' in line and 'dop ? deviceRate' in line)
if len(fields)!=3:raise SystemExit('Expected direct/native-converted/PCM modulation snapshots')
a=source.index('        const std::string orderKey = "\\\"modOrder\\\":');b=source.index('        if (!s.empty() && s.back()',a)
normalize=source[a:b]
a=source.index('            set_dsd_out_state("dop", p.dsd, dsdRate,');b=source.index(';',a)+1
initial=source[a:b]
code=r'''
#include "net/json.h"
#include <sstream>
#include <cstdio>
struct Params{int modOrder=7,dsd=64;std::string modFamily="dsd";}p;
static int captured=-1;static void set_dsd_out_state(const char*,int,unsigned,const std::string&,int order,const std::string&){captured=order;}
static std::string normalize(std::string s,int actual){struct{int outModOrder;}ps{actual};
''' +normalize+r'''
 return s;}
int main(){int checks=0,failures=0;auto check=[&](bool ok,const char* label){++checks;if(!ok)++failures;printf("%s %s\n",ok?"PASS":"FAIL",label);};
 bool dop=true;int convertedModOrder=5;unsigned dsdRate=2822400,deviceRate=176400;std::string directNote;
''' + initial+r'''
 check(captured==0,"before modulator construction effective order is unconfirmed, not the request");
 auto verify=[&](std::string text,int actual,int wantRequested){rta::net::Json j;check(rta::net::json_parse_strict(normalize(text,actual),j)&&j["modOrder"].n()==actual&&j["modOrderRequested"].n()==wantRequested,"actual JSON retains requested order separately from normalized effective order");if(j.has("carrier"))check(j["carrier"].n()==176400,"ordinary DoP snapshot records actual device carrier separately from DSD bit rate");};
 {std::ostringstream j;j<<"{\"modOrder\":7";
''' +fields[0]+r''';j<<"}";verify(j.str(),0,0);}
 {std::ostringstream j;j<<"{\"modOrder\":5";
''' +fields[1]+r''';j<<"}";verify(j.str(),5,7);}
 convertedModOrder=0;{std::ostringstream j;j<<"{\"modOrder\":0";
''' +fields[1]+r''';j<<"}";verify(j.str(),0,0);}
 {std::ostringstream j;j<<"{\"modOrder\":7";
''' +fields[2]+';'+carrier+r''';j<<"}";verify(j.str(),5,7);verify(j.str(),0,7);}
 printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
'''
compiler=sys.argv[1] if len(sys.argv)>1 else r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
with tempfile.TemporaryDirectory(prefix='rlplayer-sdm-signal-') as d:
 p=pathlib.Path(d);cpp=p/'test.cpp';exe=p/'test.exe';cpp.write_text(code,encoding='utf-8')
 subprocess.run([compiler,'-std=c++20','-O1','-I'+str(root/'src'),str(cpp),'-o',str(exe)],check=True)
 sys.exit(subprocess.run([str(exe)],cwd=d).returncode)
