"""Real private rtaweb HTTP cover contract. Never opens a playback device.

Requires an explicit built backend and FFmpeg fixture tools. All music, config,
cache and HTTP traffic belong to this disposable runtime; no user library/key.
"""
import argparse
import concurrent.futures
import ctypes
import hashlib
import http.server
import threading
import json
import os
import re
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import wave
import zlib

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def png(seed, side=32):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    state=seed
    values=[]
    for _ in range(side*side*3):
        state=(state*1664525+1013904223)&0xffffffff
        values.append(state>>24)
    raw=b"".join(b"\0"+bytes(values[y*side*3:(y+1)*side*3]) for y in range(side))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", side, side, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def fnv(text):
    h = 1469598103934665603
    for c in text.encode("utf-8"):
        h = ((h ^ c) * 1099511628211) & ((1 << 64) - 1)
    return f"{h:016x}"


def writer_priority_controls(source, compiler, evidence):
    """Compile the exact production cache IO/commit functions, with only identity/path bindings isolated."""
    evidence.mkdir()
    text=source.read_text(encoding="utf-8-sig")
    start=text.index("static std::mutex& cover_cache_mutex()")
    end=text.index("// 负缓存是否还在有效期内",start)
    logic=text[start:end]
    at=text.index("static bool cover_looks_like_image(const std::vector<unsigned char>& b) {")
    begin=text.index("{",at);depth=1;cursor=begin+1
    while depth:
        if text[cursor]=="{":depth+=1
        elif text[cursor]=="}":depth-=1
        cursor+=1
    looks=text[at:cursor]
    header=r"""
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <algorithm>
#include <iostream>
#include "net/update_check.h"
namespace fs=std::filesystem;
namespace rta::net {struct LibAlbumIdentity{bool onlineCoverEligible=true;std::string name="Fixture",artist="Artist";};}
struct Library {static Library& instance(){static Library l;return l;}bool albumIdentity(const std::string&,rta::net::LibAlbumIdentity&)const{return true;}};
static std::string fixture_root;
static std::wstring widen(const std::string& s){int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0);std::wstring w(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),w.data(),n);return w;}
static std::string pathToUtf8(const fs::path&p){auto u=p.u8string();return std::string((const char*)u.data(),u.size());}
static std::string jesc(const std::string&s){return s;}
static std::string cover_key_slug(const std::string&s){return s;}
struct CoverPaths{std::string dir,jpg,none;};
static CoverPaths cover_paths(const std::string&key){return {fixture_root,fixture_root+"/"+key+".jpg",fixture_root+"/"+key+".none"};}
static constexpr unsigned long long kCoverMaxBytes=24ull*1024*1024;
"""
    main=r"""
int main(int argc,char**argv){
 if(argc!=4)return 2;fixture_root=argv[1];fs::create_directories(fixture_root);
 auto read=[](const char*p){std::ifstream f(p,std::ios::binary);return std::vector<unsigned char>(std::istreambuf_iterator<char>(f),{});};
 auto manual=read(argv[2]),background=read(argv[3]);int failures=0;
 auto check=[&](const std::string&name,bool ok){std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";failures+=!ok;};
 for(const std::string origin:{"online","extracted"}){
  const std::string key=origin=="online"?"aaaaaaaaaaaaaaaa":"bbbbbbbbbbbbbbbb";
  check(origin+"-background-valid-before-manual",cover_write_cached(key,background,origin,"https://official.test/release/123","netease"));
  std::mutex m;std::condition_variable cv;bool queued=false,release=false,late=false;
  std::thread worker([&]{{std::unique_lock<std::mutex> l(m);queued=true;cv.notify_all();cv.wait(l,[&]{return release;});}late=cover_write_cached(key,background,origin,"https://official.test/release/123","netease");});
  {std::unique_lock<std::mutex> l(m);cv.wait(l,[&]{return queued;});}
  check(origin+"-manual-commits-after-background-queued",cover_write_cached(key,manual,"manual"));
  {std::lock_guard<std::mutex> l(m);release=true;}cv.notify_all();worker.join();
  std::vector<unsigned char> bytes;std::string source;const bool found=cover_read_cached(key,bytes,&source);
  check(origin+"-late-commit-rejected",!late);
  check(origin+"-manual-remains-committed",found&&source=="manual"&&bytes==manual);
 }
 return failures?1:0;
}
"""
    cpp=evidence/"writer.cpp";exe=evidence/"writer.exe"
    cpp.write_text(header+looks+"\n"+logic+main,encoding="utf-8")
    compiled=subprocess.run([str(compiler),"-std=c++20","-O1","-I"+str(source.parent.parent/"src"),str(cpp),"-o",str(exe)],capture_output=True)
    (evidence/"compile.log").write_bytes(compiled.stdout+compiled.stderr)
    if compiled.returncode:raise RuntimeError("Actual cache writer seam compilation failed")
    (evidence/"manual.png").write_bytes(png(51));(evidence/"background.png").write_bytes(png(53))
    result=subprocess.run([str(exe),str(evidence/"cache"),str(evidence/"manual.png"),str(evidence/"background.png")],capture_output=True,text=True,encoding="utf-8")
    (evidence/"run.log").write_text(result.stdout+result.stderr,encoding="utf-8")
    rows=[dict(case=line[5:],passed=line.startswith("PASS ")) for line in result.stdout.splitlines() if line.startswith(("PASS ","FAIL "))]
    (evidence/"results.json").write_text(json.dumps(dict(checks=rows,total=len(rows),passed=sum(x["passed"]for x in rows),exit=result.returncode,scope="Exact production cache functions; controlled identity and private path bindings; deterministic worker barrier, no HTTP/device"),indent=2),encoding="utf-8")
    print(result.stdout,end="")
    return rows


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--exe", type=Path, required=True)
    p.add_argument("--ffmpeg", type=Path, required=True)
    p.add_argument("--ffprobe", type=Path, required=True)
    p.add_argument("--product-ffmpeg", type=Path, help="Actual packaged FFmpeg; --ffmpeg remains the full fixture encoder")
    p.add_argument("--evidence-dir", type=Path, required=True)
    p.add_argument("--compiler",type=Path,default=Path("C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe"))
    p.add_argument("--writer-only",action="store_true")
    p.add_argument("--skip-lookup",action="store_true",help="Only for red regression against old binaries predating the lookup API")
    p.add_argument("--provider-fixture",action="store_true",help="Exercise real album provider HTTP using existing loopback host overrides")
    p.add_argument("--lookup-fixture",action="store_true",help="Run extra HTTP scheduling checks against a test-only backend with production test hooks")
    a = p.parse_args()
    a.evidence_dir.mkdir(parents=True, exist_ok=False)
    writer_rows=writer_priority_controls(Path(__file__).resolve().parents[1]/"tools/rtaweb.cpp",a.compiler,a.evidence_dir/"writer-priority")
    if a.writer_only:return 0 if writer_rows and all(x["passed"] for x in writer_rows) else 1
    home = a.evidence_dir / "runtime"
    home.mkdir()
    music = home / "fixture-music"
    music.mkdir()
    for source, name in [(a.exe, "rtaweb.exe"), (a.product_ffmpeg or a.ffmpeg, "ffmpeg.exe"), (a.ffprobe, "ffprobe.exe")]:
        shutil.copy2(source, home / name)
    tracks = {}
    for name in ["cached", "metastore", "directory", "embedded", "folder", "missing", "unmatched"]:
        directory = music / name
        directory.mkdir()
        wav = directory / "fixture.wav"
        with wave.open(str(wav), "wb") as f:
            f.setparams((2, 2, 44100, 0, "NONE", "not compressed"))
            f.writeframes(b"\0" * 4410 * 4)
        target = directory / "track & 中文.flac"
        args=[str(a.ffmpeg),"-v","error","-i",str(wav)]
        if name!="folder":args += ["-metadata","album=Fixture "+name]
        args += ["-metadata","artist=Fixture Artist","-metadata","title=Fixture "+name,str(target)]
        subprocess.run(args,check=True,capture_output=True)
        wav.unlink()
        tracks[name] = target
    images = {name: png(seed) for name, seed in [("cached", 17), ("metastore", 61), ("directory", 103),("embedded",141)]}
    embedded_png=tracks["embedded"].parent/"fixture-image.png";embedded_png.write_bytes(images["embedded"])
    embedded_out=tracks["embedded"].with_name("embedded.flac")
    subprocess.run([str(a.ffmpeg),"-v","error","-i",str(tracks["embedded"]),"-i",str(embedded_png),"-map","0:a","-map","1:v","-c","copy","-disposition:v","attached_pic",str(embedded_out)],check=True,capture_output=True)
    tracks["embedded"].unlink();embedded_out.replace(tracks["embedded"]);embedded_png.unlink()
    sibling=tracks["cached"].with_name("sibling.flac");shutil.copy2(tracks["cached"],sibling)
    (tracks["directory"].parent / "cover.png").write_bytes(images["directory"])
    external=home/"outside-library-music";external.mkdir()
    outside_plain=external/"temporary & 中文.flac";shutil.copy2(tracks["cached"],outside_plain)
    outside_embedded=external/"embedded & 中文.flac";shutil.copy2(tracks["embedded"],outside_embedded)
    outside_image=png(247);(external/"cover.png").write_bytes(outside_image)
    meta_covers = home / "rlplayer-meta/covers"
    meta_covers.mkdir(parents=True)
    confirmed_tracks=home/"rlplayer-meta/tracks";confirmed_tracks.mkdir()
    for path in [tracks["cached"],sibling]:
        record={"path":str(path),"cover":"","fields":{"album":{"value":"Fixture cached","source":"user","ts":1,"confidence":1}}}
        (confirmed_tracks/(fnv(str(path))+".json")).write_text(json.dumps(record),encoding="utf-8")
    (meta_covers / (fnv(str(tracks["metastore"])) + ".png")).write_bytes(images["metastore"])
    # Independent song art in an inferred group must never become album art.
    folder_embedded=tracks["folder"].with_name("own-embedded.flac")
    subprocess.run([str(a.ffmpeg),"-v","error","-i",str(tracks["embedded"]),"-c","copy","-metadata","album=",str(folder_embedded)],check=True,capture_output=True)
    folder_online=tracks["folder"].with_name("own-online.flac");shutil.copy2(tracks["folder"],folder_online)
    folder_online_image=png(249)
    (meta_covers/(fnv(str(folder_online))+".png")).write_bytes(folder_online_image)
    (tracks["folder"].parent/"cover.png").write_bytes(png(251))
    lookup_tracks={}
    if a.lookup_fixture:
        for label in ["One","Two","Three","Miss"]:
            path=tracks["folder"].with_name("lookup-"+label+".flac")
            subprocess.run([str(a.ffmpeg),"-v","error","-i",str(tracks["folder"]),"-c","copy","-metadata","title=Lookup "+label,str(path)],check=True,capture_output=True)
            lookup_tracks[label]=path
            (home/("lookup-"+label+".png")).write_bytes(png(71+len(lookup_tracks)))
    (home / "rlplayer-library.conf").write_text(str(music) + "\n", encoding="utf-8")
    (home / "rlplayer-settings.conf").write_text("summary=0\nautoOrganize=0\n", encoding="utf-8")
    profile = home / "private-profile"
    profile.mkdir()
    env = {**os.environ, "RLPLAYER_LIBRARY_DIR": str(home), "RTA_NO_DEVICE": "1", "RTA_NET_TESTS": "0",
           "USERPROFILE": str(profile), "APPDATA": str(profile), "LOCALAPPDATA": str(profile),
           "TEMP": str(profile), "TMP": str(profile), "PATH": str(home) + ";" + os.environ["SystemRoot"] + "\\System32"}
    provider_server=None;provider_requests=[]
    if a.lookup_fixture or a.provider_fixture:
        with (home/"rlplayer-settings.conf").open("a",encoding="utf-8") as config:
            config.write("metaon=on\nmetallm=off\n"+"".join("metasrc_"+sid+"="+("on" if sid=="netease" else "off")+"\n" for sid in ["netease","qq","dizzylab","itunes","discogs","musicbrainz","bgm","anilist","apple"]))
        class ProviderHandler(http.server.BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_POST(self):
                self.path+="?"+self.rfile.read(int(self.headers.get("Content-Length","0"))).decode()
                self.do_GET()
            def do_GET(self):
                provider_requests.append(self.path)
                params=urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
                term=params.get("s",[""])[0]
                if self.path.startswith("/release.png"):
                    body=png(155);mime="image/png"
                elif self.path.startswith("/api/search/get"):
                    target="Fixture metastore" if "metastore" in term else "Fixture unmatched"
                    artist="Fixture Artist" if target.endswith("metastore") else "Unrelated Artist"
                    body=json.dumps({"result":{"songs":[{"name":"Song","artists":[{"name":artist}],"album":{"name":target,"id":123,"picUrl":f"http://127.0.0.1:{self.server.server_port}/release.png"}}]}}).encode();mime="application/json"
                else:body=b'{"data":{"song":{"list":[]}}}';mime="application/json"
                self.send_response(200);self.send_header("Content-Type",mime);self.send_header("Content-Length",str(len(body)));self.end_headers();self.wfile.write(body)
        provider_server=http.server.ThreadingHTTPServer(("127.0.0.1",0),ProviderHandler)
        provider_thread=threading.Thread(target=provider_server.serve_forever,daemon=True);provider_thread.start()
        provider_base=f"http://127.0.0.1:{provider_server.server_port}"
        for variable in ["RLPLAYER_COVER_HOST","RLPLAYER_COVER_HOST_QQ","RLPLAYER_COVER_HOST_DIZZY","RLPLAYER_COVER_IMG_QQ","RLPLAYER_COVER_IMG_DIZZY"]:env[variable]=provider_base
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    base = "http://127.0.0.1:" + str(port)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def request(route, method="GET", body=None):
        req = urllib.request.Request(base + route, data=(body if body is not None else b"") if method == "POST" else None, method=method)
        try:
            with opener.open(req, timeout=10) as response:
                return response.status, response.headers.get("Content-Type", ""), response.read()
        except urllib.error.HTTPError as e:
            return e.code, e.headers.get("Content-Type", ""), e.read()

    rows = list(writer_rows)

    def check(name, condition, **detail):
        rows.append(dict(case=name, passed=bool(condition), **detail))
        print("PASS" if condition else "FAIL", name, flush=True)

    audio_before={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for directory in [music,external] for p in directory.rglob("*.flac")}
    log = (a.evidence_dir / "server.log").open("wb")
    process = subprocess.Popen([str(home / "rtaweb.exe"), "--port", str(port)], cwd=home, env=env, stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("Private backend exited before readiness")
            try:
                albums = json.loads(request("/api/lib/albums")[2])["albums"]
                if len(albums) == len(tracks):
                    break
            except (OSError, ValueError):
                pass
            time.sleep(0.1)
        else:
            raise RuntimeError("Private fixture scan did not finish")
        keys = {name: next(x["k"] for x in albums if x["name"] == ("folder" if name=="folder" else "Fixture "+name)) for name in tracks}
        cache = home / "covercache"
        cache.mkdir(exist_ok=True)
        (cache / (keys["cached"] + ".jpg")).write_bytes(images["cached"])
        for name in ["embedded","directory"]:(cache/(keys[name]+".jpg")).write_bytes(png(249))
        before_get={str(p.relative_to(home)):hashlib.sha256(p.read_bytes()).hexdigest() for directory in [cache,home/"rlplayer-meta"] for p in directory.rglob("*") if p.is_file()}
        for name, track in tracks.items():
            query = urllib.parse.quote(str(track), safe="")
            metadata = json.loads(request("/api/meta?file=" + query+"&_cov=1")[2])
            status, mime, body = request("/api/cover?file=" + query+"&_cov=2")
            expected = name in ["metastore","embedded","directory"]
            if name=="cached":check("untyped-old-cache-does-not-prove-release",metadata.get("coverSource")=="none")
            check(name + "-meta-image-agreement", bool(metadata.get("hasCover")) == expected,
                  hasCover=metadata.get("hasCover"), image_status=status, image_bytes=len(body))
            if expected:
                url=metadata.get("coverUrl","")
                check(name+"-canonical-cover-url", bool(url) and request(url)[2]==images[name],url=url)
                check(name + "-real-image-bytes", status == 200 and body == images[name], mime=mime,
                      sha256=hashlib.sha256(body).hexdigest())
            else:
                check(name + "-missing-is-empty-404", status == 404 and not body, status=status, bytes=len(body))
        for label,path,image in [("embedded",folder_embedded,images["embedded"]),("online",folder_online,folder_online_image)]:
            q=urllib.parse.quote(str(path),safe="");md=json.loads(request("/api/meta?file="+q)[2])
            check("unmatched-folder-independent-song-"+label,md.get("hasCover") is True and request("/api/cover?file="+q)[2]==image and request(md.get("coverUrl","/missing"))[2]==image)
        check("inferred-folder-album-keeps-its-own-local-embedded",request("/api/lib/cover?k="+keys["folder"])[2]==images["embedded"])
        # Album/track share the exact canonical cache, and cache-buster query args are not part of identity.
        st, _, album_image = request("/api/lib/cover?k=" + keys["cached"] + "&_cov=1&_r=2")
        check("unmatched-album-rejects-old-cache", st == 404 and not album_image)
        for label,path,image in [("directory",outside_plain,outside_image),("own-embedded",outside_embedded,images["embedded"])]:
            q=urllib.parse.quote(str(path),safe="")
            md=json.loads(request("/api/meta?file="+q+"&_cov=11")[2]);status,_,body=request("/api/cover?file="+q)
            check("outside-"+label+"-real-image",status==200 and body==image,status=status,bytes=len(body))
            check("outside-"+label+"-metadata-agrees",md.get("hasCover") is True and bool(md.get("coverUrl")) and request(md["coverUrl"])[2]==image)
        current_albums=json.loads(request("/api/lib/albums")[2])["albums"]
        for name,identity,eligible in [("cached","user_assigned",True),("folder","directory_inferred",False)]:
            row=next(x for x in current_albums if x["k"]==keys[name]);detail=json.loads(request("/api/lib/album?k="+keys[name])[2])
            check(name+"-album-identity-contract",all(x.get("albumIdentitySource")==identity and x.get("onlineCoverEligible") is eligible for x in [row,detail]))

        for name,expected_source in [("embedded","embed"),("directory","dir"),("metastore","none")]:
            row=next(x for x in current_albums if x["k"]==keys[name])
            album=json.loads(request("/api/lib/album?k="+keys[name])[2])
            status,_,image=request("/api/lib/cover?k="+keys[name])
            expected=expected_source!="none"
            check(name+"-list-source-matches-actual-local",row.get("cvSrc")==expected_source and album.get("cvSrc")==expected_source and row.get("cv") is expected and album.get("cv") is expected and (status==200 and image==images[name] if expected else status==404 and not image),list_source=row.get("cvSrc"),album_source=album.get("cvSrc"))
        after_get={str(p.relative_to(home)):hashlib.sha256(p.read_bytes()).hexdigest() for directory in [cache,home/"rlplayer-meta"] for p in directory.rglob("*") if p.is_file()}
        check("GET-does-not-write-cover-or-metastore",before_get==after_get)
        # Real process IO bounds the list path: one valid ~192 KiB picture must
        # not be fully read on every metadata-only album-list request.
        legacy_path=cache/(keys["cached"]+".jpg")
        legacy_saved=legacy_path.read_bytes();legacy_path.write_bytes(png(119,256))
        class IoCounters(ctypes.Structure):
            _fields_=[(n,ctypes.c_ulonglong) for n in ["read_ops","write_ops","other_ops","read_bytes","write_bytes","other_bytes"]]
        io_kernel=ctypes.WinDLL("kernel32",use_last_error=True)
        io_kernel.GetProcessIoCounters.argtypes=[ctypes.c_void_p,ctypes.POINTER(IoCounters)]
        def read_counter():
            counter=IoCounters()
            if not io_kernel.GetProcessIoCounters(int(process._handle),ctypes.byref(counter)):raise ctypes.WinError(ctypes.get_last_error())
            return counter.read_bytes
        request("/api/lib/albums");before_io=read_counter()
        for _ in range(3):request("/api/lib/albums")
        list_reads=read_counter()-before_io
        check("album-list-reads-bounded-headers-not-whole-images",list_reads<3*32768,read_bytes=list_reads,requests=3,large_image_bytes=legacy_path.stat().st_size)
        legacy_path.write_bytes(b"INVALID_HEADER!!"+legacy_saved[16:])
        listed=next(x for x in json.loads(request("/api/lib/albums")[2])["albums"] if x["k"]==keys["cached"])
        check("changed-legacy-header-invalidates-list-hint",listed.get("cvSrc")=="none")
        legacy_path.write_bytes(legacy_saved)
        listed=next(x for x in json.loads(request("/api/lib/albums")[2])["albums"] if x["k"]==keys["cached"])
        check("restored-legacy-still-does-not-prove-release",listed.get("cvSrc")=="none")

        key=keys["cached"];track_query=urllib.parse.quote(str(tracks["cached"]),safe="")
        manifest=cache/(key+".jpg.source.json")
        def upload(image):
            return json.loads(request("/api/lib/cover/set?k="+key+"&_cov=3","POST",image)[2])
        manual=png(191)
        check("manual-set-commit",upload(manual).get("ok") is True)
        info=json.loads(manifest.read_text(encoding="utf-8")) if manifest.is_file() else {}
        blob=cache/info.get("blob","missing")
        check("manual-origin-and-content-bound",info.get("source")=="manual" and info.get("sha256")==hashlib.sha256(manual).hexdigest() and blob.is_file() and blob.read_bytes()==manual)
        check("manual-preserves-legacy-user-image",(cache/(key+".jpg")).read_bytes()==images["cached"])
        check("manual-track-album-meta-agree",request("/api/cover?file="+track_query)[2]==manual and request("/api/lib/cover?k="+key)[2]==manual and json.loads(request("/api/meta?file="+track_query)[2]).get("coverSource")=="manual")
        if manifest.is_file():
            committed=manifest.read_bytes()
            # Real OS sharing denial: reads remain legal; atomic replace must fail.
            kernel=ctypes.WinDLL("kernel32",use_last_error=True)
            kernel.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_void_p,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_void_p]
            kernel.CreateFileW.restype=ctypes.c_void_p
            kernel.CloseHandle.argtypes=[ctypes.c_void_p]
            handle=kernel.CreateFileW(str(manifest),0x80000000,1,None,3,0x80,None)
            if handle in [None,ctypes.c_void_p(-1).value]:raise OSError("Cannot establish private manifest sharing denial")
            try:
                rejected=upload(png(193))
                check("source-commit-denied-is-failure",rejected.get("ok") is False)
                check("failed-source-commit-preserves-old-manifest",manifest.read_bytes()==committed)
                check("failed-source-commit-preserves-readable-image",request("/api/cover?file="+track_query)[2]==manual)
            finally:kernel.CloseHandle(handle)
            exclusive=kernel.CreateFileW(str(manifest),0x80000000,0,None,3,0x80,None)
            if exclusive in [None,ctypes.c_void_p(-1).value]:raise OSError("Cannot establish exclusive source lock")
            try:
                listed=next(x for x in json.loads(request("/api/lib/albums")[2])["albums"] if x["k"]==key)
                check("locked-source-record-does-not-fall-back-to-legacy",listed.get("cvSrc")=="none" and request("/api/cover?file="+track_query)[0]==404)
            finally:kernel.CloseHandle(exclusive)
            manifest.write_text("{broken",encoding="utf-8")
            missing=json.loads(request("/api/meta?file="+track_query)[2])
            check("corrupt-manifest-does-not-resurrect-legacy",missing.get("hasCover") is False and request("/api/cover?file="+track_query)[0]==404)
            manifest.write_bytes(committed)
            blob.write_bytes(b"corrupted")
            check("bad-blob-digest-rejected",json.loads(request("/api/meta?file="+track_query)[2]).get("hasCover") is False and request("/api/cover?file="+track_query)[0]==404)
            blob.write_bytes(manual)
            # An online record is still exact-key/digest bound, never inferred by extension.
            info["source"]="online";manifest.write_text(json.dumps(info),encoding="utf-8")
            check("old-v1-online-is-not-release-proof",request("/api/cover?file="+track_query)[0]==404 and request("/api/lib/cover?k="+key)[0]==404)
            info.update(version=2,albumName="Fixture cached",albumArtist="Fixture Artist",matchUrl="https://official.test/release/123",matchProvider="netease")
            manifest.write_text(json.dumps(info),encoding="utf-8")
            check("typed-online-cache-readable",request("/api/cover?file="+track_query)[2]==manual and json.loads(request("/api/meta?file="+track_query)[2]).get("coverSource")=="online")
            info["source"]="extracted";manifest.write_text(json.dumps(info),encoding="utf-8")
            check("typed-extracted-cache-not-release-match",request("/api/cover?file="+track_query)[0]==404 and request("/api/lib/cover?k="+key)[0]==404 and json.loads(request("/api/meta?file="+track_query)[2]).get("coverSource")=="none")
            typed=manifest.read_bytes()
            for label,patch in [("wrong-key",{"key":keys["missing"]}),("path-traversal",{"blob":"../user.jpg"}),("unsupported-origin",{"source":"guessed"}),("wrong-bytes",{"bytes":len(manual)+1})]:
                original_stamp=manifest.stat()
                bad={**info,**patch};manifest.write_text(json.dumps(bad),encoding="utf-8")
                if label=="wrong-key":os.utime(manifest,ns=(original_stamp.st_atime_ns,original_stamp.st_mtime_ns))
                check("source-record-rejects-"+label,json.loads(request("/api/meta?file="+track_query)[2]).get("hasCover") is False and request("/api/cover?file="+track_query)[0]==404 and next(x for x in json.loads(request("/api/lib/albums")[2])["albums"] if x["k"]==key).get("cvSrc")=="none")
            manifest.write_bytes(b" "*8193)
            check("source-record-budget-rejects-oversize",json.loads(request("/api/meta?file="+track_query)[2]).get("hasCover") is False and request("/api/cover?file="+track_query)[0]==404)
            manifest.write_bytes(typed)

            candidates=[png(i) for i in range(201,209)]
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                outcomes=list(pool.map(upload,candidates))
            final=json.loads(manifest.read_text(encoding="utf-8"));actual=request("/api/cover?file="+track_query)[2]
            check("concurrent-manual-writers-all-commit",all(x.get("ok") is True for x in outcomes))
            check("concurrent-final-image-matches-single-manifest",actual in candidates and final["sha256"]==hashlib.sha256(actual).hexdigest() and final["source"]=="manual")
            snapshot=manifest.read_bytes()
            oversized=b"\x89PNG\r\n\x1a\n"+b"x"*(24*1024*1024)
            try:response=request("/api/lib/cover/set?k="+key,"POST",oversized)
            except OSError:response=(0,"",b"")
            try:accepted=json.loads(response[2]).get("ok") is True
            except ValueError:accepted=False
            check("oversized-upload-refused-preserves-commit",not accepted and manifest.read_bytes()==snapshot,status=response[0])
            budget=cache/(key+"."+"e"*64+".blob")
            with budget.open("wb") as f:f.truncate(128*1024*1024)
            try:check("managed-cache-budget-refuses-preserves-commit",upload(png(211)).get("ok") is False and manifest.read_bytes()==snapshot)
            finally:budget.unlink()

        # Folder groups remain browsable but do not manufacture release metadata or inherit unidentified online images.
        folderq=urllib.parse.quote(str(tracks["folder"]),safe="")
        folder_meta=json.loads(request("/api/meta?file="+folderq)[2])
        check("folder-label-is-not-release-album",not folder_meta.get("album") and folder_meta.get("albumIdentitySource")=="directory_inferred" and folder_meta.get("folderName")=="folder")
        folderlegacy=cache/(keys["folder"]+".jpg");folderlegacy.write_bytes(png(221))
        check("folder-rejects-legacy-and-keeps-owned-embedded-card",json.loads(request("/api/meta?file="+folderq)[2]).get("hasCover") is False and request("/api/lib/cover?k="+keys["folder"])[2]==images["embedded"])
        folder_set=json.loads(request("/api/lib/cover/set?k="+keys["folder"],"POST",png(223))[2])
        check("explicit-folder-manual-is-exact-group-member-fallback",folder_set.get("ok") is True and request("/api/lib/cover?k="+keys["folder"])[2]==png(223) and request("/api/cover?file="+folderq)[2]==png(223) and json.loads(request("/api/meta?file="+folderq)[2]).get("albumIdentitySource")=="directory_inferred")
        track_image=png(231)
        trackset=json.loads(request("/api/cover/set?file="+track_query+"&_cov=8","POST",track_image)[2])
        check("track-specific-manual-commit",trackset.get("ok") is True and trackset.get("coverSource")=="track_manual")
        trackmeta=json.loads(request("/api/meta?file="+track_query)[2])
        check("track-specific-url-and-meta-agree",trackmeta.get("coverSource")=="track_manual" and bool(trackmeta.get("coverUrl")) and request(trackmeta["coverUrl"])[2]==track_image)
        track_records=list((home/"trackcovercache").glob("*.source.json"))
        check("track-independent-namespace",len(track_records)==1 and json.loads(track_records[0].read_text())["key"].startswith("track:"))
        track_record=track_records[0];track_saved=track_record.read_bytes();track_info=json.loads(track_saved)
        track_info["key"]=key;track_record.write_text(json.dumps(track_info),encoding="utf-8")
        check("track-record-cannot-be-album-record",json.loads(request("/api/meta?file="+track_query)[2]).get("hasCover") is False and request("/api/cover?file="+track_query)[0]==404)
        track_record.write_bytes(track_saved)

        check("track-override-does-not-change-album",request("/api/lib/cover?k="+key)[2]==actual)
        siblingq=urllib.parse.quote(str(sibling),safe="")
        check("track-override-does-not-change-sibling",request("/api/cover?file="+siblingq)[2]==actual)
        handle=kernel.CreateFileW(str(track_record),0x80000000,1,None,3,0x80,None)
        if handle in [None,ctypes.c_void_p(-1).value]:raise OSError("Cannot establish track reset sharing denial")
        try:
            reset_failed=json.loads(request("/api/cover/reset?file="+track_query,"POST")[2])
            check("track-reset-commit-failure-preserves-override",reset_failed.get("ok") is False and track_record.read_bytes()==track_saved and request("/api/cover?file="+track_query)[2]==track_image)
        finally:kernel.CloseHandle(handle)
        before_blobs=set((home/"trackcovercache").glob("*.blob"))
        reset=json.loads(request("/api/cover/reset?file="+track_query,"POST")[2])
        check("track-reset-returns-automatic-source",reset.get("ok") is True and request("/api/cover?file="+track_query)[2]==actual and json.loads(request("/api/meta?file="+track_query)[2]).get("coverSource")!="track_manual")
        check("track-reset-preserves-immutable-blobs",before_blobs==set((home/"trackcovercache").glob("*.blob")))
        embq=urllib.parse.quote(str(tracks["embedded"]),safe="")
        check("track-manual-overrides-own-embedded",json.loads(request("/api/cover/set?file="+embq,"POST",track_image)[2]).get("ok") is True and request("/api/cover?file="+embq)[2]==track_image)
        check("track-reset-restores-own-embedded",json.loads(request("/api/cover/reset?file="+embq,"POST")[2]).get("ok") is True and request("/api/cover?file="+embq)[2]==images["embedded"])
        outside=home/"outside.wav";outside.write_bytes(b"not in library")
        for label,path in [("outside-library",outside),("not-existing",home/"missing.flac")]:
            q=urllib.parse.quote(str(path),safe="")
            check("track-set-rejects-"+label,json.loads(request("/api/cover/set?file="+q,"POST",track_image)[2]).get("ok") is False)
        check("track-cover-set-GET-is-405",request("/api/cover/set?file="+track_query)[0]==405)
        check("track-cover-reset-GET-is-405",request("/api/cover/reset?file="+track_query)[0]==405)
        check("album-cover-set-GET-is-405",request("/api/lib/cover/set?k="+key)[0]==405)
        # Album art is only a fallback for songs lacking independent artwork.
        same_embedded=tracks["cached"].parent/"same-album-embedded.flac"
        subprocess.run([str(a.ffmpeg),"-v","error","-i",str(tracks["embedded"]),"-map","0","-c","copy","-metadata","album=Fixture cached",str(same_embedded)],check=True,capture_output=True)
        same_meta=tracks["cached"].parent/"same-album-metastore.flac";shutil.copy2(tracks["cached"],same_meta)
        individual_meta_image=png(251)
        (meta_covers/(fnv(str(same_meta))+".png")).write_bytes(individual_meta_image)
        same_blank=tracks["cached"].parent/"same-album-unconfirmed-blank.flac";shutil.copy2(tracks["cached"],same_blank)
        request("/api/lib/scan","POST",b"full=1")
        deadline=time.monotonic()+15
        while time.monotonic()<deadline:
            album=json.loads(request("/api/lib/album?k="+key)[2])
            if len(album.get("tracks",[]))==5:break
            time.sleep(.05)
        else:raise RuntimeError("Same-album fixture scan did not complete")
        for label,path,expected_image in [("own-embedded",same_embedded,images["embedded"]),("own-metastore",same_meta,individual_meta_image)]:
            q=urllib.parse.quote(str(path),safe="")
            md=json.loads(request("/api/meta?file="+q)[2])
            check("album-manual-does-not-override-song-"+label,request("/api/cover?file="+q)[2]==expected_image and bool(md.get("coverUrl")) and request(md["coverUrl"])[2]==expected_image)
        check("album-manual-still-serves-album",request("/api/lib/cover?k="+key)[2]==actual)
        check("same-key-member-without-own-art-inherits-explicit-group-manual",request("/api/cover?file="+urllib.parse.quote(str(same_blank),safe=""))[2]==actual)
        check("confirmed-song-without-own-art-inherits-album-manual",request("/api/cover?file="+siblingq)[2]==actual)
        check("no-published-temporary-source",not any(cache.glob("*.tmp-*")))
        check("all-original-audio-bytes-unchanged",all(Path(p).is_file() and hashlib.sha256(Path(p).read_bytes()).hexdigest()==digest for p,digest in audio_before.items()))
        check("reordered-file-query-decodes-once",request("/api/cover?v=123&file="+embq+"&_cov=9")[2]==images["embedded"] and json.loads(request("/api/meta?v=123&file="+embq)[2]).get("hasCover") is True)

        # Provider misses cannot suppress already owned local album artwork.
        # This does not promote a song-only Meta image or an inferred group.
        (cache/(keys["embedded"]+".none")).write_bytes(b"")
        negrow=next(x for x in json.loads(request("/api/lib/albums")[2])["albums"] if x["k"]==keys["embedded"])
        negdetail=json.loads(request("/api/lib/album?k="+keys["embedded"])[2])
        check("provider-negative-preserves-owned-local-album",negrow.get("cvSrc")=="embed" and negdetail.get("cvSrc")=="embed" and request("/api/lib/cover?k="+keys["embedded"])[2]==images["embedded"] and request("/api/cover?file="+embq)[2]==images["embedded"] and (cache/(keys["embedded"]+".none")).is_file())
        # User-confirmed date and disc/track fractions retain their full format.
        metatracks=home/"rlplayer-meta/tracks";metatracks.mkdir(exist_ok=True)
        confirmed={"date":"2015-02-25","trackNo":"1/11","discNo":"2/2"}
        record={"path":str(tracks["missing"]),"cover":"","fields":{k:{"value":v,"source":"user","ts":1,"confidence":1} for k,v in confirmed.items()}}
        (metatracks/(fnv(str(tracks["missing"]))+".json")).write_text(json.dumps(record),encoding="utf-8")
        missingq=urllib.parse.quote(str(tracks["missing"]),safe="")
        confirmed_meta=json.loads(request("/api/meta?file="+missingq)[2])
        check("user-confirmed-full-date-disc-track",confirmed_meta.get("date")=="2015-02-25" and confirmed_meta.get("track")=="1/11" and confirmed_meta.get("disc")=="2/2")
        tagged_image=png(253)
        tagged_set=json.loads(request("/api/lib/cover/set?k="+keys["missing"],"POST",tagged_image)[2])
        check("tagged-group-manual-fallback-keeps-identity",tagged_set.get("ok") is True and request("/api/lib/cover?k="+keys["missing"])[2]==tagged_image and request("/api/cover?file="+missingq)[2]==tagged_image and json.loads(request("/api/meta?file="+missingq)[2]).get("albumIdentitySource")=="tagged")
        if provider_server:
            for label in ["unmatched","metastore"]:
                queued=json.loads(request("/api/lib/cover/prefetch?k="+keys[label],"POST")[2])
                check("tagged-own-art-does-not-suppress-album-provider-"+label,queued.get("queued")==1)
                deadline=time.monotonic()+25
                while time.monotonic()<deadline:
                    progress=json.loads(request("/api/lib/cover/progress")[2])
                    if not progress.get("running"):break
                    time.sleep(.1)
                if label=="unmatched":
                    check("real-provider-name-hit-wrong-artist-normal-jpg-rejected",request("/api/lib/cover?k="+keys[label])[0]==404 and request("/api/cover?file="+urllib.parse.quote(str(tracks[label]),safe=""))[0]==404)
                else:
                    proof=json.loads((cache/(keys[label]+".jpg.source.json")).read_text(encoding="utf-8"))
                    check("real-provider-matched-release-has-v2-provenance",request("/api/lib/cover?k="+keys[label])[2]==png(155) and proof.get("version")==2 and proof.get("albumName")=="Fixture metastore" and proof.get("albumArtist")=="Fixture Artist" and proof.get("matchProvider")=="netease")
                    ownq=urllib.parse.quote(str(tracks[label]),safe="")
                    check("matched-album-never-replaces-own-song-art",request("/api/cover?file="+ownq)[2]==images[label])
        if a.lookup_fixture:
            def lookup(label):return json.loads(request("/api/cover/lookup?file="+urllib.parse.quote(str(lookup_tracks[label]),safe=""),"POST")[2])
            one=lookup("One");duplicate=lookup("One");two=lookup("Two");three=lookup("Three")
            check("lookup-enqueue-dedupe-and-global-two-worker-cap",one.get("pending") and duplicate.get("pending") and two.get("pending") and three.get("busy") is True)
            poll_start=time.monotonic();request("/api/meta?file="+missingq)
            check("lookup-workers-do-not-block-read-http",time.monotonic()-poll_start<1)
            for label in ["One","Two"]:
                until=time.monotonic()+10
                while time.monotonic()<until:
                    state=lookup(label)
                    if not state.get("pending") and (not state.get("hasCover") or state.get("coverSource")=="track_online"):break
                    time.sleep(.1)
                expected=(home/("lookup-"+label+".png")).read_bytes()
                check("lookup-real-http-song-result-"+label,state.get("hasCover") is True and state.get("coverSource")=="track_online" and request(state.get("coverUrl","/missing"))[2]==expected,state=state)
            check("lookup-result-does-not-populate-unmatched-album",request("/api/lib/cover?k="+keys["folder"])[2]==png(223) and request("/api/cover?file="+folderq)[0]==404)
            before_lines=(home/"lookup-fixture.log").read_text(encoding="utf-8").splitlines()
            check("lookup-same-song-only-one-search",sum(line=="SEARCH Lookup One" for line in before_lines)==1)
            miss=lookup("Miss");until=time.monotonic()+10
            while miss.get("pending") and time.monotonic()<until:
                time.sleep(.1);miss=lookup("Miss")
            count_before=(home/"lookup-fixture.log").read_bytes();again=lookup("Miss")
            check("lookup-miss-negative-dedupes-repeated-requests",miss.get("hasCover") is False and miss.get("pending") is False and again.get("pending") is False and (home/"lookup-fixture.log").read_bytes()==count_before)
            before_get_net=(home/"lookup-fixture.log").read_bytes()
            request("/api/meta?file="+missingq);request("/api/cover?file="+missingq);request("/api/lib/albums");request("/api/lib/album?k="+keys["folder"]);request("/api/lib/cover?k="+keys["folder"])
            check("read-only-http-does-not-query-any-network-source",(home/"lookup-fixture.log").read_bytes()==before_get_net)
        if not a.skip_lookup:
            # The lazy lookup write endpoint respects privacy and never opens audio.
            (home/"rlplayer-settings.conf").write_text("summary=0\nautoOrganize=0\nmetaon=0\n",encoding="utf-8")
            disabledq=urllib.parse.quote(str(tracks["unmatched"]),safe="")
            disabled=json.loads(request("/api/cover/lookup?file="+disabledq,"POST")[2])
            check("track-lookup-disabled-no-enqueue",disabled.get("ok") is True and disabled.get("disabled") is True and disabled.get("pending") is False)
            check("track-lookup-GET-is-405",request("/api/cover/lookup?file="+missingq)[0]==405)
            for label,path in [("outside",outside_plain),("missing",music/"not-existing.flac")]:
                q=urllib.parse.quote(str(path),safe="")
                check("track-lookup-rejects-"+label,json.loads(request("/api/cover/lookup?file="+q,"POST")[2]).get("ok") is False)
            hit=json.loads(request("/api/cover/lookup?file="+embq,"POST")[2])
            check("track-lookup-local-hit-no-enqueue",hit.get("hasCover") is True and hit.get("pending") is False and request(hit.get("coverUrl","/missing"))[2]==images["embedded"])

    finally:
        if process.poll() is None:
            request("/api/quit", "POST")
        process.wait(timeout=15)
        log.close()
        if provider_server:provider_server.shutdown();provider_server.server_close();provider_thread.join()
        (a.evidence_dir / "results.json").write_text(json.dumps(dict(checks=rows, passed=sum(x["passed"] for x in rows),
            total=len(rows), server_exit=process.returncode, port=port, exe_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),
            no_playback_requests=True, fixture_only=True), ensure_ascii=False, indent=2), encoding="utf-8")
    return 0 if rows and all(x["passed"] for x in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
