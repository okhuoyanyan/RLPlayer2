"""Run the production ASIO selection function without loading/opening drivers."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compiler", default="C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe")
parser.add_argument("--evidence", type=Path, default=Path("asio-selection340-evidence"))
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / "tools/rtaweb.cpp").read_text(encoding="utf8")
selection = source[source.index("static bool is_asio_clsid("):source.index("// ASIO：设备 id")]
sink = source[source.index("class AsioSink :"):source.index("class NativeDsdSink :")]
assert sink.index("resolve_asio_selection(o.device") < sink.index("asio_preflight("), "selection must precede every hardware probe"
unit = '#include "audio/asio_out.h"\n#include <cstdio>\n' + selection + r'''
int main(){
 const std::string a="{12345678-1234-1234-1234-1234567890AB}",b="{12345678-1234-1234-1234-1234567890AC}";
 const std::vector<rta::audio::AsioDriverEntry> drivers={{"wanted",a,false},{"other",b,false},{"broken","invalid",false}};
 int checks=0;std::string selected,error;
 auto run=[&](const std::string& request,bool expected,const std::string& destination){
  ++checks;selected="previous";error="previous";
  const bool ok=resolve_asio_selection(request,drivers,selected,error);
  return ok==expected&&selected==destination&&(ok?error.empty():!error.empty());
 };
 if(!run("",true,"")||!run("wanted",true,a)||!run(a,true,a)||!run("missing",false,"")||
    !run("{0.0.0.00000000}."+a,false,"")||!run("broken",false,"")||!run(b,true,b))return 1;
 std::printf("ASIO_SELECTION checks=%d failures=0 audioOpened=0\n",checks);return 0;
}
'''
destination = args.evidence.resolve()
destination.mkdir(parents=True, exist_ok=False)
cpp, exe = destination / "selection.cpp", destination / "test_asio_selection340.exe"
cpp.write_text(unit, encoding="utf8")
built = subprocess.run([args.compiler, "-std=c++23", "-O2", "-I", str(root / "src"), str(cpp), "-o", str(exe), "-lole32", "-luuid"],
                       capture_output=True, timeout=90)
(destination / "build.log").write_bytes(built.stdout + built.stderr)
built.check_returncode()
tested = subprocess.run([str(exe)], capture_output=True, timeout=15)
(destination / "run.log").write_bytes(tested.stdout + tested.stderr)
tested.check_returncode()
assert b"ASIO_SELECTION checks=7 failures=0 audioOpened=0" in tested.stdout
report = {"checks": 7, "failures": 0, "audioOpened": False,
          "production_function_sha256": hashlib.sha256(selection.encode()).hexdigest(), "evidence": str(destination)}
(destination / "results.json").write_text(json.dumps(report, indent=2), encoding="utf8")
print(json.dumps(report), flush=True)
