"""Exercise actual workbench restarts in a private Null-only player."""
import argparse
import ctypes
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import wave


def verify_capability_planner(root, compiler):
    """Compile the production planner body against controlled device replies.

    The live API tests below use the real Null backend. This separate adapter
    checks hardware capability combinations that the local Null device cannot
    advertise, without loading or opening an audio driver.
    """
    source_root = Path(__file__).resolve().parents[1]
    production = (source_root / "src/net/workbench_config.h").read_text(encoding="utf8")
    planner = production[production.index("inline bool plan_profile("):production.index("inline std::string apply(")]
    unit = r'''
#include "engine/preset_profiles.h"
#include <algorithm>
#include <iostream>
#include <map>
namespace rta::audio {
struct MediaInfo { int sampleRate=44100,bits=24; };
static int testRate=44100;
MediaInfo probe_media(const std::string&){return {testRate,24};}
std::string lowerExt(const std::string& file){return file=="dsd"?".dsf":".wav";}
}
namespace rta::dsp { struct PeqSettings { bool bypass=false; }; }
struct PlayParams {
 std::string file,rate,filter,filterNx,sdmFilter,sdmFilterNx,shaper,phase,modFamily,eqFile,convIr,rgMode;
 int taps=0,dsd=0,twoStage=-1,modOrder=5;
 double atten=0,transition=0,volume=-67.25,trim[8]={};
 bool dsdNative=false,directSdm=false,convGpu=false,loudness=false,isp=false,roomCorrAuto=false,cleanup=false;
};
struct FakeDsd { uint32_t rate=2822400; };
FakeDsd probe_dsd(const std::string&){return {};}
static rta::engine::DeviceCaps testCaps;
rta::engine::DeviceCaps preset_probe_caps(const PlayParams*){return testCaps;}
using Values=std::map<std::string,std::string>;
'''
    unit += planner
    unit += r'''
int main(){
 int checks=0;
 auto run=[&](const char* file,int rate,const std::vector<uint32_t>& rates,int expected){
  testCaps={};testCaps.backend="asio";testCaps.rates=rates;
  testCaps.maxRate=*std::max_element(rates.begin(),rates.end());testCaps.dsdNative=true;
  testCaps.dsdNativeMax="dsd512";testCaps.dop=true;testCaps.dopMax="dsd256";
  rta::audio::testRate=rate;PlayParams candidate;candidate.file=file;
  rta::dsp::PeqSettings peq;std::string why;
  const bool accepted=plan_profile({{"profile","dsd"}},candidate,peq,why);
  if(expected>0 ? (!accepted||candidate.dsd!=expected||candidate.dsdNative) : accepted){
   std::cerr<<"Capability decision failed: source="<<rate<<" expected="<<expected
            <<" got="<<candidate.dsd<<" accepted="<<accepted<<" why="<<why;return false;
  }
  ++checks;return true;
 };
 if(!run("pcm",44100,{44100,176400,352800,705600},256))return 1;
 if(!run("dsd",44100,{44100,176400,352800,705600},256))return 1;
 if(!run("pcm",48000,{48000,192000,384000,705600},128))return 1;
 if(!run("pcm",48000,{48000,192000,384000,768000},256))return 1;
 if(!run("pcm",44100,{44100,88200},0))return 1;
 if(!run("pcm",96000,{44100,88200,176400},0))return 1;
 std::cout<<"CAPABILITY_CASES="<<checks<<" failures=0\n";return 0;
}
'''
    cpp = root / "workbench-capability.cpp"
    exe = root / "test_workbench_caps.exe"
    cpp.write_text(unit, encoding="utf8")
    build = subprocess.run([str(compiler), "-std=c++23", "-O2", "-I", str(source_root / "src"),
                            str(cpp), "-o", str(exe)], capture_output=True, timeout=90)
    (root / "capability-build.log").write_bytes(build.stdout + build.stderr)
    build.check_returncode()
    checked = subprocess.run([str(exe)], capture_output=True, timeout=10)
    (root / "capability-results.log").write_bytes(checked.stdout + checked.stderr)
    checked.check_returncode()
    assert b"CAPABILITY_CASES=6 failures=0" in checked.stdout
    return {"cases": 6, "production_function_sha256": hashlib.sha256(planner.encode("utf8")).hexdigest()}


def verify_raw_control_contract(root, compiler):
    """Execute the real gates and volume handoff against in-memory state only."""
    source_root = Path(__file__).resolve().parents[1]
    production = (source_root / "tools/rtaweb.cpp").read_text(encoding="utf8")
    gates = production[production.index("static bool raw_peq_requested("):production.index("// 直接播放 DSD 源：")]
    unit = r'''
#include "engine/pipeline.h"
#include "dsp/direct_sdm.h"
#include <atomic>
#include <barrier>
#include <iostream>
#include <mutex>
#include <thread>
namespace eng=rta::engine;
using rta::audio::DsdInfo;using rta::audio::probe_dsd;
namespace rta::audio {std::string lowerExt(const std::string& s){return s.substr(s.find_last_of('.'));}}
struct PlayParams {
 std::string backend="asio",file="private.dsf",eqFile,convIr,rate="src",rgMode="off";
 double volume=0,trim[8]={};int dsd=64,outChannels=0;
 bool dsdNative=false,directSdm=true,loudness=false,cleanup=false,roomCorrAuto=false,isp=false;
};
struct State {std::mutex mtx;std::atomic<bool> playing{true};bool rawVolumeLocked=false;
 std::atomic<double> volDb{0},startDb{0};} g_ps;
static std::shared_ptr<rta::dsp::PeqEngine> g_peq=std::make_shared<rta::dsp::PeqEngine>();
'''
    unit += gates
    unit += r'''
int main(){
 int checks=0,races=0;auto check=[&](bool ok,const char* label){++checks;if(!ok){std::cerr<<"FAIL "<<label<<"\n";std::exit(2);}};
 DsdInfo info;info.rate=2822400;info.channels=2;std::string why;
 auto flat=rta::dsp::peq_flat_settings();g_peq->set_settings(flat);
 auto both=[&](const PlayParams& p,bool expected,const char* label){
  check(native_dsd_precheck(p,why)==expected,label);check(direct_sdm_eligible(p,info,why)==expected,label);
 };
 PlayParams p;both(p,true,"unity and flat PEQ is eligible");
 for(double db:{-67.25,-20.0,-0.5,-1e-12}){p.volume=db;both(p,false,"negative gain cannot enter raw");}p.volume=0;
 auto peq=flat;peq.bypass=false;peq.preampDb=-3;g_peq->set_settings(peq);both(p,false,"active PEQ preamp forbids raw");
 check(native_dsd_precheck(p,why,&flat),"candidate bypass replaces previous live processing");
 g_peq->set_settings(flat);check(!native_dsd_precheck(p,why,&peq),"candidate PEQ is checked before commit");
 peq=flat;peq.bypass=false;peq.bands[3].gainDb=-2;g_peq->set_settings(peq);both(p,false,"active PEQ band forbids raw");
 peq.bands[3].enabled=false;g_peq->set_settings(peq);both(p,true,"disabled PEQ band is harmless");
 peq.bypass=true;peq.preampDb=-3;g_peq->set_settings(peq);both(p,true,"bypassed PEQ is harmless");
 g_peq->set_settings(flat);
 for(const char* rg:{"track","album"}){p.rgMode=rg;both(p,false,"ReplayGain requests processing");}p.rgMode="off";
 p.roomCorrAuto=true;both(p,false,"automatic room correction requests processing");p.roomCorrAuto=false;
 p.isp=true;both(p,false,"peak protection requests processing");p.isp=false;
 p.loudness=true;both(p,false,"loudness requests processing");p.loudness=false;
 p.cleanup=true;both(p,false,"cleanup requests processing");p.cleanup=false;
 p.eqFile="eq";both(p,false,"file EQ requests processing");p.eqFile.clear();
 p.convIr="ir";both(p,false,"IR requests processing");p.convIr.clear();
 p.trim[0]=-.25;both(p,false,"channel gain requests processing");p.trim[0]=0;
 p.outChannels=1;check(!direct_sdm_eligible(p,info,why),"channel conversion forbids direct");p.outChannels=0;
 check(raw_dsd_pending(p),"pending raw claim is armed before decode");
 for(bool native:{false,true}){
  p.dsdNative=native;
  g_ps.rawVolumeLocked=false;g_ps.volDb=0;
  {std::lock_guard<std::mutex> lock(g_ps.mtx);check(write_live_volume_locked(-70),"volume-first request is accepted");}
  PlayParams late=p;check(!claim_raw_dsd_volume(late,&info,why)&&late.volume==-70,"raw handoff sees previously accepted gain");
  g_ps.rawVolumeLocked=false;g_ps.volDb=0;
  check(claim_raw_dsd_volume(p,&info,why),"raw-first handoff claims unity volume");
  {std::lock_guard<std::mutex> lock(g_ps.mtx);check(!write_live_volume_locked(-70)&&g_ps.volDb==0,"raw-first rejects later gain without mutation");}
  for(int n=0;n<200;++n){
   g_ps.rawVolumeLocked=false;g_ps.volDb=0;PlayParams candidate=p;candidate.volume=0;
   bool wrote=false,raw=false;std::barrier start(3);
   std::thread writer([&]{start.arrive_and_wait();std::lock_guard<std::mutex> lock(g_ps.mtx);wrote=write_live_volume_locked(-70);});
   std::thread owner([&]{start.arrive_and_wait();std::string error;raw=claim_raw_dsd_volume(candidate,&info,error);});
   start.arrive_and_wait();writer.join();owner.join();
   check(wrote!=raw&&(wrote?g_ps.volDb==-70&&!g_ps.rawVolumeLocked:g_ps.volDb==0&&g_ps.rawVolumeLocked),"startup ordering cannot silently lose accepted gain");++races;
  }
 }
 std::cout<<"RAW_CONTROL_CASES="<<checks<<" STARTUP_RACES="<<races<<" failures=0\n";
}
'''
    cpp, exe = root / "workbench-raw-control.cpp", root / "test_workbench_raw_control.exe"
    cpp.write_text(unit, encoding="utf8")
    built = subprocess.run([str(compiler), "-std=c++23", "-O2", "-I", str(source_root / "src"),
                            str(cpp), "-o", str(exe), "-lole32", "-luuid"], capture_output=True, timeout=90)
    (root / "raw-control-build.log").write_bytes(built.stdout + built.stderr)
    built.check_returncode()
    result = subprocess.run([str(exe)], capture_output=True, timeout=15)
    (root / "raw-control-results.log").write_bytes(result.stdout + result.stderr)
    report = {"exit": result.returncode, "production_functions_sha256": hashlib.sha256(gates.encode()).hexdigest(),
              "stdout": result.stdout.decode("utf8", "replace"), "stderr": result.stderr.decode("utf8", "replace")}
    (root / "raw-control-results.json").write_text(json.dumps(report, indent=2), encoding="utf8")
    result.check_returncode()
    assert "STARTUP_RACES=400 failures=0" in report["stdout"], report
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path)
    parser.add_argument("--evidence", type=Path)
    parser.add_argument("--make-dsd", type=Path)
    parser.add_argument("--compiler", type=Path, default=Path("C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe"))
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix="rlp-workbench-"))
    capability_report = verify_capability_planner(root, args.compiler)
    exe = root / "rtaweb.exe"
    shutil.copy2(args.exe, exe)
    for parent in (args.exe.parent, args.exe.parent / "ffmpeg-lite"):
        for name in ("ffmpeg.exe", "ffprobe.exe"):
            if (parent / name).is_file() and not (root / name).exists():
                shutil.copy2(parent / name, root / name)
    config = root / "rlplayer-settings.conf"
    config.write_text(
        "backend=null\ndevice=null\nvolume=-67.25\nrate=88200\nfilter=poly-sinc-short-lp\n"
        "gpuvendor=off\nmetaon=0\nmetallm=0\nroomcorrauto=0\nisp=0\n"
        "anlmaster=0\nautoanl=0\nquickstart=0\n", encoding="utf8")
    (root / "rlplayer-playlist.txt").write_text("# RLPlayer playlist v1\nindex=0\nrepeat=0\nshuffle=0\n", encoding="utf8")
    library = root / "library"
    library.mkdir()
    empty = root / "empty-music"
    empty.mkdir()
    (library / "rlplayer-library.conf").write_text(str(empty) + "\n", encoding="utf8")
    track = root / "long-silence.wav"
    next_track = root / "next-silence.wav"
    rate48_track = root / "48k-silence.wav"
    for file, seconds, sample_rate in ((track, 60, 44100), (next_track, 2, 44100), (rate48_track, 60, 48000)):
        with wave.open(str(file), "wb") as output:
            output.setparams((2, 2, sample_rate, 0, "NONE", ""))
            for _ in range(seconds):
                output.writeframes(bytes(sample_rate * 4))
    helper = args.make_dsd or args.exe.parent / "make_dsd.exe"
    if not helper.is_file():
        raise FileNotFoundError("DSD-source regression requires --make-dsd or a neighboring make_dsd.exe")
    generator = root / "make_dsd.exe"
    shutil.copy2(helper, generator)
    dsd_track = root / "quiet-dsd64.dsf"
    generated = subprocess.run([str(generator), "--out", str(dsd_track), "--seconds", "6", "--amp", "-75"],
                               cwd=root, capture_output=True, timeout=60, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    (root / "dsd-fixture.log").write_bytes(generated.stdout + generated.stderr)
    if generated.returncode or not dsd_track.is_file():
        raise AssertionError("Offline DSD fixture generation failed")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = "http://127.0.0.1:" + str(port)
    checks = []
    measurements = {}
    raw_control_report = None

    def check(condition, label):
        checks.append({"name": label, "ok": bool(condition)})
        print(("PASS " if condition else "FAIL ") + label, flush=True)
        if not condition:
            raise AssertionError(label)

    def request(path, payload=None, method=None):
        data = None if payload is None else urllib.parse.urlencode(payload).encode("utf8")
        req = urllib.request.Request(base + path, data=data, method=method)
        try:
            with urllib.request.urlopen(req, timeout=30) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as response:
            return response.code, json.load(response)

    def get(path="/api/workbench/state"):
        status, body = request(path)
        assert status == 200 and body.get("ok"), (status, body)
        return body

    def until(predicate, seconds=15):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                value = predicate()
                if value:
                    return value
            except (OSError, AssertionError):
                pass
            time.sleep(.025)
        raise AssertionError("Timed out waiting for actual player state")

    def apply(patch, state=None):
        state = state or get()
        return request("/api/workbench/apply", {
            "expected_file": state["file"], "expected_revision": state["revision"], **patch})

    def play(file, **extra):
        values = dict(file=str(file), backend="null", device="null", volume="-67.25", rate="88200",
                      filter="poly-sinc-short-lp", buffer="20", gpuvendor="off", roomcorrauto="0", isp="0")
        values.update(extra)
        status, result = request("/api/play", values)
        assert status == 200 and result.get("ok"), result
        return until(lambda: (lambda s: s if s["playing"] and s["duration"] > 0 and s["position"] > .15 else None)(get()))

    def pause_current():
        status, result = request("/api/pause?on=1", {})
        assert status == 200 and result.get("ok"), result
        requested = get()
        observations = []

        def acknowledged():
            actual = get("/api/status")
            observations.append({"paused": actual["paused"], "position": actual["pos"]})
            return actual if actual["playing"] and actual["paused"] and actual["current"] == requested["file"] else None

        # Workbench paused includes pauseReq so the UI can show intent at once.
        # Only status.paused confirms the renderer has finished an in-flight
        # write and stopped. Capture the comparison position after that ack.
        actual = until(acknowledged)
        paused = get()
        measurements.setdefault("pause_observations", []).append({
            "file": requested["file"], "requested_position": requested["position"],
            "acknowledged_position": actual["pos"], "stable_position": paused["position"],
            "status_polls": observations,
        })
        assert paused["playing"] and paused["paused"] and paused["file"] == requested["file"]
        assert abs(paused["position"] - actual["pos"]) < .00001
        return paused

    out = (root / "backend.log").open("wb")
    env = dict(os.environ, RLPLAYER_LIBRARY_DIR=str(library), RLPLAYER_UPDATE_DIR=str(root / "update"),
               RLPLAYER_UPDATE_MANIFEST="http://127.0.0.1:1/manifest.json", RLPLAYER_UPDATE_ALLOW_LOCAL="1",
               RTA_NO_DEVICE="1", RTA_NET_TESTS="0", RLPLAYER_FILTER_CACHE="0")
    proc = subprocess.Popen([str(exe), "--port", str(port)], cwd=root, env=env,
                            stdout=out, stderr=out, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    try:
        print("Null sink only, two private all-zero WAV files, saved volume -67.25 dB, no hardware.", flush=True)
        initial = until(lambda: get())
        check(initial["settings"]["backend"] == "null" and initial["volume"] == -67.25,
              "read-only snapshot restores saved endpoint and fractional volume")
        check("volume" not in initial["settings"] and initial["settings"]["ui_style"] == "balanced",
              "configuration snapshots exclude volume and identify a genuine style")
        status, custom = apply({"rate": "src"}, initial)
        check(status == 200 and custom["profile"] == "custom" and custom["mode"] == "custom",
              "source-rate processing is identified as custom instead of claiming PCM upsampling")
        status, initial = apply({"rate": "88200"}, custom)
        assert status == 200
        check(request("/api/workbench/apply")[0] == 405, "configuration writes reject GET")
        check(request("/api/workbench/apply", {"rate": "176400"})[0] == 400, "unguarded writes are rejected")
        for patch, label in (({"volume": "0"}, "volume override"), ({"filter": "missing-filter"}, "unknown filter"),
                             ({"rate": "NaN"}, "NaN rate"), ({"rate": "Infinity"}, "infinite rate"),
                             ({"modorder": "3.5"}, "fractional order"), ({"isp": "yes"}, "invalid boolean"),
                             ({"modfamily": "asdm"}, "unsupported modulator family"),
                             ({"peq": '{"bypass":0}'}, "truncated PEQ snapshot")):
            status, result = apply(patch, initial)
            check(status == 400 and not result["ok"], "rejects " + label)
        check(get()["revision"] == initial["revision"], "invalid requests leave the authoritative state unchanged")
        status, dsd = apply({"profile": "dsd"}, initial)
        check(status == 200 and int(dsd["settings"]["dsd"]) >= 64 and dsd["settings"]["dsdnative"] == "0",
              "DSD profile supports a PCM source through the existing PCM-to-DSD plan")
        status, _ = apply({"profile": "pcm", "style": "warm"}, dsd)
        check(status == 200 and get()["settings"]["filter"] == "poly-sinc-gauss-medium",
              "listening style saves the real Gaussian filter")
        current = play(track)
        # A lock that permits reading but denies replacement reproduces a real
        # configuration-save failure without changing any filesystem ACL.
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        create = kernel.CreateFileW
        create.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                           ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        create.restype = ctypes.c_void_p
        close = kernel.CloseHandle
        close.argtypes = [ctypes.c_void_p]
        close.restype = ctypes.c_int
        for locked, label in ((config, "settings"), (root / "rlplayer-peq.txt", "PEQ")):
            handle = create(str(locked), 0x80000000, 1, None, 3, 0, None)
            assert handle not in (None, ctypes.c_void_p(-1).value)
            try:
                state = get()
                config_before = config.read_bytes()
                status, failure = apply({"rate": "192000"}, state)
                check(status == 500 and not failure["ok"] and get()["playing"]
                      and get()["revision"] == state["revision"] and config.read_bytes() == config_before,
                      label + " persistence failure keeps the running session and prior configuration")
            finally:
                assert close(handle)
        paused = pause_current()
        peq = paused["peq"]
        peq["bypass"] = 0
        peq["preamp"] = -3.0
        peq["name"] = "Private AB test"
        peq["bands"][3]["gain"] = -2.5
        old_pos = paused["position"]
        status, switched = apply({"rate": "176400", "filter": "poly-sinc-gauss-medium", "peq": json.dumps(peq)}, paused)
        check(status == 200 and switched["ok"], "paused filter/rate/EQ snapshot is accepted atomically")
        ready = until(lambda: (lambda s: s if s["paused"] and s["duration"] > 0 else None)(get()))
        # Persist numeric evidence before the assertion, including failed runs.
        measurements.update(paused_position_before=old_pos, paused_position_after=ready["position"],
                            paused_gain_before=paused["volume"], paused_gain_after=ready["volume"],
                            paused_apply_position=switched["position"], paused_apply_gain=switched["volume"])
        check(abs(ready["position"] - old_pos) < .03 and ready["volume"] == -67.25,
              "paused restart retains position and exact live gain")
        time.sleep(.25)
        paused_advance = get()["position"] - ready["position"]
        measurements["paused_advance_over_250ms"] = paused_advance
        check(abs(paused_advance) < .005,
              "initially paused restart writes no track frames before resume")
        check(ready["settings"]["rate"] == "176400" and ready["peq"]["bands"][3]["gain"] == -2.5,
              "actual session returns the chosen rate and all PEQ bands")
        no_name = dict(ready["peq"])
        no_name.pop("name")
        status, _ = apply({"peq": json.dumps(no_name)}, ready)
        check(status == 200 and get()["peq"]["name"] == "Private AB test",
              "unnamed EQ updates retain the current descriptive name")
        check(apply({"rate": "88200"}, paused)[0] == 409, "a stale configuration cannot overwrite the new snapshot")
        request("/api/volume?db=-68.75", {})
        request("/api/pause?on=0", {})
        current = until(lambda: (lambda s: s if not s["paused"] and s["position"] > old_pos + .1 else None)(get()))
        status, _ = apply({"rate": "88200", "filter": "poly-sinc-short-lp", "position": "2.0"}, current)
        check(status == 200, "pinned same-passage comparison accepts an explicit track-relative start")
        resumed = until(lambda: (lambda s: s if s["playing"] and s["duration"] > 0 and s["position"] >= 2 else None)(get()))
        check(resumed["position"] < 2.5 and resumed["volume"] == -68.75,
              "active restart begins at the pinned passage and keeps the new live gain")
        measurements.update(pinned_target=2.0, pinned_observed=resumed["position"],
                            active_gain_after=resumed["volume"])
        revision = get()
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(lambda rate: apply({"rate": rate}, revision)[0], ("176400", "192000")))
        check(sorted(results) == [200, 409], "two concurrent A/B clicks cannot both spend the same revision")
        request("/api/stop", {})
        check(not get()["playing"], "owned Null session stops cooperatively")
        request("/api/clear", {})
        # A one-second CUE segment naturally advances to the next queue item.
        request("/api/queue/set", {"files": str(track) + "\t3\t4\n" + str(next_track)})
        cue = play(track, cue_start="3", cue_end="4", queueIndex="0")
        cue = pause_current()
        status, _ = apply({"filter": "poly-sinc-gauss-short", "position": ".35"}, cue)
        check(status == 200, "CUE comparison applies a position within its own segment")
        cue_after = until(lambda: (lambda s: s if s["paused"] and s["duration"] > 0 else None)(get()))
        check(cue_after["cueStart"] == 3 and cue_after["cueEnd"] == 4 and cue_after["queueIndex"] == 0
              and abs(cue_after["position"] - .35) < .03 and abs(cue_after["duration"] - 1) < .02,
              "CUE boundaries, index and relative position survive configuration changes")
        measurements["cue"] = {k: cue_after[k] for k in ("cueStart", "cueEnd", "queueIndex", "position", "duration")}
        request("/api/pause?on=0", {})
        second = until(lambda: (lambda s: s if s["file"] == str(next_track) and s["playing"] and s["duration"] > 0 else None)(get()))
        check(second["position"] < .3 and not second["paused"] and second["cueStart"] == -1,
              "the next song inherits neither the CUE range nor the pinned start/pause state")
        check(apply({"rate": "176400"}, cue_after)[0] == 409, "a delayed response from the previous song is rejected")
        request("/api/stop", {})
        stopped = get()
        pcm48 = play(rate48_track)
        pcm48 = pause_current()
        status, dsd48 = apply({"profile": "dsd"}, pcm48)
        actual48 = get("/api/status")
        check(status == 200 and actual48["outRate"] == 48000 * int(dsd48["settings"]["dsd"]) / 16
              and dsd48["settings"]["dsdnative"] == "0",
              "48k PCM-to-DSD output uses its actual 48k-family DoP carrier")
        measurements["pcm48_to_dsd"] = {"level": int(dsd48["settings"]["dsd"]), "carrier": actual48["outRate"]}
        request("/api/stop", {})
        dsd_current = play(dsd_track)
        dsd_current = pause_current()
        status, dsd_up = apply({"profile": "dsd", "style": "warm"}, dsd_current)
        check(status == 200 and int(dsd_up["settings"]["dsd"]) > 64
              and dsd_up["settings"]["directsdm"] == "0"
              and dsd_up["settings"]["filter"] == "poly-sinc-gauss-medium",
              "a DSD source uses actual DSD-to-DSD upsampling and its selected style")
        signal = get("/api/signal")["path"]
        check(signal["dsd"] == int(dsd_up["settings"]["dsd"]) and not signal["directSdm"]
              and abs(dsd_up["position"] - dsd_current["position"]) < .03 and dsd_up["paused"],
              "DSD-to-DSD output is prepared before success and retains the paused passage")
        measurements["dsd64_to_dsd"] = {"level": signal["dsd"], "position": dsd_up["position"],
                                          "paused": dsd_up["paused"], "directSdm": signal["directSdm"]}
        status, original_dsd = apply({"profile": "bitperfect"}, dsd_up)
        check(status == 200 and original_dsd["profile"] == "bitperfect"
              and original_dsd["settings"]["directsdm"] == "1" and original_dsd["volume"] == -67.25,
              "original DSD mode remains distinguishable from upsampling while preserving software gain")
        request("/api/stop", {})
        stopped = get()
        status, result = apply({"backend": "upnp", "upnpdev": "http://127.0.0.1:1/description.xml"}, stopped)
        check(status == 200 and result["settings"]["backend"] == "upnp"
              and result["settings"]["upnpdev"] == "http://127.0.0.1:1/description.xml",
              "stopped network-output selection saves and returns its renderer endpoint")
        # A genuine raw DSF request uses the Null sink; the fixture is -75 dBFS.
        # Observe the control contract immediately after /play, including its
        # preparation window, instead of waiting until playback is established.
        stopped = get()
        flat = dict(stopped["peq"])
        flat["bypass"] = 1
        status, _ = apply({"backend": "null", "device": "null", "peq": json.dumps(flat)}, stopped)
        assert status == 200
        raw_form = dict(file=str(dsd_track), backend="null", device="null", volume="0", rate="src",
                        filter="", shaper="none", dsd="64", directsdm="1", dsdnative="0", buffer="20",
                        gpuvendor="off", roomcorrauto="0", isp="0", rgmode="off", cleanup="0")
        status, started = request("/api/play", raw_form)
        assert status == 200 and started.get("ok"), started
        raw_pending = get()
        volume_status, volume_result = request("/api/volume?db=-72.5", {})
        raw_after = get()
        measurements["raw_volume_request"] = {
            "before": {key: raw_pending.get(key) for key in ("playing", "position", "duration", "volume", "volumeWritable", "rawVolumeLocked")},
            "response_status": volume_status, "response": volume_result,
            "after": {key: raw_after.get(key) for key in ("playing", "position", "duration", "volume", "volumeWritable", "rawVolumeLocked")},
        }
        check(volume_status == 409 and not volume_result["ok"] and raw_after["volume"] == 0,
              "raw DSD preparation or playback refuses negative software gain without changing live volume")
        check(raw_pending.get("rawVolumeLocked") is True and raw_pending.get("volumeWritable") is False
              and raw_after.get("rawVolumeLocked") is True and raw_after.get("volumeWritable") is False,
              "raw DSD publishes its software-volume restriction before and after preparation")
        raw = pause_current()
        raw_actual = get("/api/status")
        raw_signal = get("/api/signal")["path"]
        measurements["raw_actual"] = {"status": {key: raw_actual.get(key) for key in
            ("rawVolumeLocked", "volumeWritable", "volDb", "startDb", "outMode")},
            "directSdm": raw_signal.get("directSdm"), "position": raw["position"]}
        check(raw_actual.get("rawVolumeLocked") is True and raw_actual.get("volumeWritable") is False
              and raw_actual["volDb"] == 0 and raw_actual["startDb"] == 0
              and raw_signal.get("directSdm") is True and raw_actual["outMode"] == "dop",
              "actual raw DoP publishes truthful format and unity-only volume state")
        status, zero = request("/api/volume?db=0", {})
        check(status == 200 and zero["ok"] and get()["volume"] == 0,
              "a unity-volume request is harmless and idempotent on raw DSD")
        before_peq = get()
        status, peq_failure = request("/api/peq", {"bypass": "0", "preamp": "-3"})
        check(status == 409 and not peq_failure["ok"] and get()["peq"] == before_peq["peq"],
              "legacy PEQ writes cannot silently enable processing on a raw stream")
        processed_peq = dict(before_peq["peq"])
        processed_peq["bypass"], processed_peq["preamp"] = 0, -3
        status, peq_processed = apply({"peq": json.dumps(processed_peq)}, before_peq)
        peq_signal = get("/api/signal")["path"]
        check(status == 200 and peq_processed["paused"] and peq_processed["mode"] == "custom"
              and peq_processed.get("rawVolumeLocked") is False and peq_processed.get("volumeWritable") is True
              and peq_signal.get("directSdm") is False and peq_signal.get("bitPerfect") not in ("yes", "direct"),
              "a same-snapshot PEQ request makes DirectSDM use the real processing path and custom mode")
        status, peq_gain = request("/api/volume?db=-72.5", {})
        check(status == 200 and peq_gain["ok"] and get()["volume"] == -72.5,
              "processed DoP accepts negative software gain after the raw-to-PEQ switch")
        # A deliberately nonexistent ASIO identity prevents any audio-driver
        # startup even if a future regression skips this candidate PEQ gate.
        request("/api/volume?db=0", {})
        current = get()
        bypassed = dict(current["peq"])
        bypassed["bypass"] = 1
        status, _ = apply({"peq": json.dumps(bypassed)}, current)
        assert status == 200
        before_native = get()
        config_before_native = config.read_bytes()
        status, native_failure = apply({"backend": "asio", "device": "{00000000-0000-0000-0000-000000000001}",
                                       "dsdnative": "1", "peq": json.dumps(processed_peq)}, before_native)
        measurements["native_candidate_peq"] = {"status": status, "response": native_failure,
            "unchanged_revision": get()["revision"] == before_native["revision"]}
        check(status == 400 and not native_failure["ok"] and "PEQ" in native_failure["error"]
              and get()["revision"] == before_native["revision"] and config.read_bytes() == config_before_native,
              "Native validates candidate PEQ before saving or touching the active Null endpoint")
        for key, value in (("rgmode", "track"), ("roomcorrauto", "1"), ("isp", "1")):
            status, processed = apply({"directsdm": "1", "dsdnative": "0", "rgmode": "off",
                                      "roomcorrauto": "0", "isp": "0", key: value}, get())
            signal = get("/api/signal")["path"]
            measurements.setdefault("direct_processing", []).append({"request": {key: value},
                "status": status, "mode": processed.get("mode"), "rawVolumeLocked": processed.get("rawVolumeLocked"),
                "volumeWritable": processed.get("volumeWritable"), "directSdm": signal.get("directSdm")})
            check(status == 200 and processed["paused"] and processed["mode"] == "custom"
                  and processed.get("rawVolumeLocked") is False and processed.get("volumeWritable") is True
                  and signal.get("directSdm") is False,
                  key + " forces a real processing path instead of an ignored raw request")
            status, _ = request("/api/volume?db=-72.5", {})
            check(status == 200 and get()["volume"] == -72.5,
                  key + " processing retains writable software volume")
            request("/api/volume?db=0", {})
        request("/api/stop", {})
        stopped = get()
        check(stopped.get("volumeWritable") is True and stopped.get("rawVolumeLocked") is False,
              "stopping a raw or processed session restores software-volume controls")
        raw_control_report = verify_raw_control_contract(root, args.compiler)
    finally:
        try:
            request("/api/quit", {})
        except OSError:
            pass
        proc.wait(timeout=20)
        out.close()
        report = {"checks": checks, "passed": sum(c["ok"] for c in checks), "total": len(checks),
                  "capability_matrix": capability_report,
                  "raw_control_matrix": raw_control_report,
                  "measurements": measurements,
                  "private": str(root), "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
                  "audio_device": "null", "audible_output": False, "process_exit": proc.returncode}
        destination = args.evidence or root
        destination.mkdir(parents=True, exist_ok=True)
        (destination / "workbench-results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf8")
        print(json.dumps(report, ensure_ascii=False), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
