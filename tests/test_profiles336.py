"""Private real-server persistence/profile/EQ/CUE-segment regression; Null only.

Run via D:/RLPlayer-DevTools/firewall/Start-RLPlayerGuard.ps1.
No physical device is enumerated/opened, no user configuration is copied.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import shutil
import socket
import stat
import struct
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import wave


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("exe", type=Path)
    parser.add_argument("--run-root", type=Path)
    parser.add_argument("--short-loop-only", action="store_true")
    parser.add_argument("--skip-waveform-retry", action="store_true")
    args = parser.parse_args()
    default_root=Path(__file__).resolve().parents[1]/'build336-evidence'/('profiles336-'+uuid.uuid4().hex)
    root = (args.run_root or default_root).resolve()
    root.mkdir(parents=True, exist_ok=False)
    exe = root / "rtaweb.exe"
    shutil.copy2(args.exe, exe)
    library = root / "library"
    library.mkdir()
    empty = root / "empty-music"
    empty.mkdir()
    (library / "rlplayer-library.conf").write_text(str(empty) + "\n", encoding="utf8")
    (root / "rlplayer-settings.conf").write_text(
        "backend=null\ndevice=null\nvolume=-67.25\nrate=44100\nfilter=\nshaper=none\n"
        "gpuvendor=off\nmetaon=0\nmetallm=0\nroomcorrauto=0\nisp=0\nanlmaster=0\nautoanl=0\nquickstart=0\n",
        encoding="utf8")
    (root / "rlplayer-playlist.txt").write_text("# RLPlayer playlist v1\nindex=0\nrepeat=0\nshuffle=0\n", encoding="utf8")
    track = root / "cue-source.wav"
    other = root / "other.wav"
    for file in (track, other):
        with wave.open(str(file), "wb") as wav:
            wav.setparams((2, 2, 44100, 0, "NONE", ""))
            for second in range(30):
                amp = (30, 10, 20, 15)[second - 3] if file == track and 3 <= second < 7 else 0
                wav.writeframes(struct.pack("<hh", amp, -amp) * 44100)
    metadata = root / "profiles/opra/metadata"
    metadata.mkdir(parents=True)
    opra = [{"id": "test-preset", "productId": "test::headphone", "author": "Fixture author",
             "details": "Measured target", "link": "https://example.invalid/source",
             "parameters": {"gain_db": -4.2, "bands": [
                 {"type": "peak_dip", "frequency": 1000, "gain_db": 2.5, "q": 1.2},
                 {"type": "low_shelf", "frequency": 80, "gain_db": 3, "q": .71}]}}]
    (metadata / "test__headphone.json").write_text(json.dumps(opra), encoding="utf8")
    bad = [{**opra[0], "id": "unsupported", "productId": "bad::headphone",
            "parameters": {"bands": [{"type": "low_pass", "frequency": 1000, "q": 1}]}}]
    (metadata / "bad__headphone.json").write_text(json.dumps(bad), encoding="utf8")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    env = dict(os.environ, RTA_NO_DEVICE="1", RTA_NET_TESTS="0", RLPLAYER_FILTER_CACHE="0",
               RLPLAYER_LIBRARY_DIR=str(library), RLPLAYER_UPDATE_DIR=str(root / "update"),
               RLPLAYER_UPDATE_MANIFEST="http://127.0.0.1:1/manifest.json", RLPLAYER_UPDATE_ALLOW_LOCAL="1")
    checks, measurements = [], {}
    proc = None
    log = None

    def request(path, payload=None, method=None, timeout=35):
        body = None if payload is None else urllib.parse.urlencode(payload).encode()
        req = urllib.request.Request(base + path, data=body, method=method)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as response:
            return response.code, json.load(response)

    def get(path="/api/workbench/state"):
        status, body = request(path)
        assert status == 200 and body.get("ok"), (status, body)
        return body

    def post(path, **values):
        status, body = request(path, values)
        assert status == 200 and body.get("ok"), (path, status, body)
        return body

    def until(predicate, seconds=12):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                value = predicate()
                if value:
                    return value
            except OSError:
                pass
            time.sleep(.015)
        raise AssertionError("Timed out waiting for actual renderer")

    def check(ok, label):
        checks.append({"name": label, "ok": bool(ok)})
        print(("PASS " if ok else "FAIL ") + label, flush=True)
        assert ok, label

    def profiles():
        return get("/api/workbench/profiles")

    def group(id="A"):
        return next(p for p in profiles()["slots"] if p["id"] == id)

    def peq_snapshot(peq):
        # Live replies also contain sample rate, presets and a response curve;
        # compare every saved audio parameter rather than those derived views.
        return {k: json.loads(json.dumps(peq[k])) for k in ("bypass", "preamp", "name", "bands")}

    def settings_status_storm():
        active = profiles()["active"]
        auto_before = group(active)["settings"]["cleanup_auto"]
        quick_before = get("/api/settings")["kv"]["quickstart"]
        barrier = threading.Barrier(5, timeout=8)
        failed, writers_done = threading.Event(), threading.Event()
        done_lock, done_count = threading.Lock(), [0]
        started = time.monotonic()
        deadline = started + 20

        def participant(kind):
            result = {"kind": kind, "calls": 0, "errors": [], "max_ms": 0}
            writer = kind in ("profile", "settings")
            try:
                barrier.wait()
                while not failed.is_set():
                    n = result["calls"]
                    if writer and n == 40:
                        break
                    if not writer and n >= 100 and writers_done.is_set():
                        break
                    if time.monotonic() >= deadline:
                        raise TimeoutError("20-second concurrent handler budget exceeded")
                    if kind == "profile":
                        path, payload = "/api/workbench/profile", {"action": "update", "id": active,
                            "cleanup_auto": auto_before if n == 39 else str(n % 2)}
                    elif kind == "settings":
                        path, payload = "/api/settings", {"quickstart": quick_before if n == 39 else str(n % 2)}
                    else:
                        path, payload = "/api/status", None
                    begin = time.monotonic()
                    code, reply = request(path, payload, timeout=6)
                    result["max_ms"] = max(result["max_ms"], (time.monotonic()-begin)*1000)
                    result["calls"] += 1
                    if code != 200 or not reply.get("ok"):
                        result["errors"].append({"status": code, "error": reply.get("error", "missing ok")})
                        failed.set()
            except Exception as error:
                result["errors"].append({"exception": type(error).__name__, "error": str(error)})
                failed.set()
            finally:
                if writer:
                    with done_lock:
                        done_count[0] += 1
                        if done_count[0] == 2:
                            writers_done.set()
            return result

        # Five clients leave a server worker available for cooperative /api/quit
        # even if a lock-order regression makes these requests time out.
        with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
            futures = [pool.submit(participant, kind) for kind in ("profile", "settings", "status1", "status2", "status3")]
            rows = [future.result() for future in futures]
        elapsed = time.monotonic()-started
        measurements["settings_status_concurrency"] = {"participants": rows, "elapsed_seconds": elapsed,
            "request_timeout_seconds": 6, "total_budget_seconds": 20, "physical_device_opened": False}
        writers, readers = rows[:2], rows[2:]
        check(all(row["calls"] == 40 and not row["errors"] for row in writers),
              "real concurrent profile and legacy settings writes all commit without HTTP 500")
        check(all(row["calls"] >= 100 and not row["errors"] for row in readers) and elapsed < 20,
              "parallel real status readers complete within the bounded budget without lock inversion")
        check(group(active)["settings"]["cleanup_auto"] == auto_before
              and get("/api/settings")["kv"]["quickstart"] == quick_before
              and get()["settings"]["backend"] == "null" and get()["volume"] == -67.25,
              "concurrent handlers retain both committed settings and the authoritative group safely")

    def pause():
        post("/api/pause?on=1")
        until(lambda: get("/api/status")["paused"])
        return get()

    def start():
        nonlocal proc, log
        log = (root / f"server-{len(measurements)}.log").open("wb")
        proc = subprocess.Popen([str(exe), "--port", str(port)], cwd=root, env=env,
                                stdout=log, stderr=log,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        return until(lambda: get())

    def quit():
        nonlocal proc, log
        if proc is not None:
            if proc.poll() is None:
                try:
                    post("/api/quit")
                except OSError:
                    pass
                proc.wait(timeout=20)
            measurements.setdefault("exit_codes", []).append(proc.returncode)
            proc = None
        if log is not None:
            log.close()
            log = None

    try:
        initial = start()
        if not args.skip_waveform_retry:
            retry_track = root / "waveform-retry.wav"
            valid_wave = track.read_bytes()
            retry_track.write_bytes(b"BAD!" + valid_wave[4:])
            retry_stat = retry_track.stat()
            retry_path = "/api/waveform?" + urllib.parse.urlencode({"file": str(retry_track), "clip_length": "1"})
            retry_error = until(lambda: (lambda w: w if w.get("state") == "error" else None)(request(retry_path)[1]))
            check(bool(retry_error.get("error")), "waveform retry fixture first reaches a genuine decoder error")
            retry_track.write_bytes(valid_wave)
            os.utime(retry_track, ns=(retry_stat.st_atime_ns, retry_stat.st_mtime_ns))
            check(retry_track.stat().st_size == retry_stat.st_size and retry_track.stat().st_mtime_ns == retry_stat.st_mtime_ns
                  and request(retry_path)[1].get("state") == "error", "ordinary polling retains the same-key two-second waveform error cache")
            retried = request(retry_path + "&fresh=1")[1]
            check(retried.get("state") == "pending", "immediate explicit HTTP retry bypasses the cached error and really queues decoding")
            retry_ready = until(lambda: (lambda w: w if w.get("state") == "ready" else None)(get(retry_path)))
            check(abs(retry_ready["duration"] - 1) < .0001 and request(retry_path + "&fresh=1")[1].get("state") == "ready",
                  "retried repaired source decodes successfully and fresh reuses a ready result")
            measurements["waveform_retry"] = {"initial_error": retry_error["error"], "fresh_state": retried["state"],
                                              "final_state": retry_ready["state"], "duration": retry_ready["duration"],
                                              "source_size_and_mtime_unchanged": True}
            check(request(retry_path + "&fresh=true")[0] == 400, "waveform fresh accepts only explicit zero or one")
        post("/api/queue/set", files=str(track) + "\t3\t3.3")
        post("/api/play", file=str(track), backend="null", device="null", volume="-67.25", rate="44100",
             filter="", shaper="none", gpuvendor="off", roomcorrauto="0", isp="0", cue_start="3", cue_end="3.3", queueIndex="0")
        short = pause()
        check(abs(short["duration"] - .3) < .001, "short CUE fixture is a real 300ms production stream")
        post("/api/workbench/loop", enabled="1", start=".05", end=".15", repeat="1", file=str(track))
        post("/api/pause?on=0")
        time.sleep(.65)
        short_after = get()
        check(short_after["playing"] and .049 <= short_after["position"] <= .1501,
              "fragment replay remains active after a short source has fully decoded")
        post("/api/workbench/loop", enabled="0")
        post("/api/stop")
        post("/api/clear")
        if args.short_loop_only:
            return
        p = profiles()
        check(p["active"] == "A" and [s["id"] for s in p["slots"]] == list("ABCD"), "four complete server-owned groups initialize")
        # 2026-11-15：backend/device/buffer/upnpdev 现在是**每组**的输出路由（用户要求 A–D 可接不同设备），
        # 不再属于"全局传输字段"。仍然排除的是实时音量、曲目身份与 GPU 能力（全局）。
        forbidden = {"volume", "file", "gpuvendor"}
        check(all(len(s["settings"]) >= 35 and not forbidden.intersection(s["settings"])
                  and s["settings"].get("backend") and "device" in s["settings"]
                  and len(s["peq"]["bands"]) == 20 for s in p["slots"]),
              "every group has full DSP/PEQ state plus its own output route, excluding live volume and track identity")
        check(request("/api/workbench/profile")[0] == 405, "profile mutations reject GET")
        migrated = post("/api/workbench/profile", action="migrate", id="B", name="旧槽 B", rate="88200")
        post("/api/workbench/profile", action="migrate", id="B", name="overwrite", rate="96000")
        check(group("B")["settings"]["rate"] == "88200" and group("B")["name"] == "旧槽 B", "old localStorage migration is idempotent")
        legacy_peq = peq_snapshot(get()["peq"])
        legacy_peq.update(bypass=False, preamp=-5.75)
        legacy_peq["bands"][0].update(on=True, freq=2100, gain=-2.25, q=1.15)
        post("/api/workbench/profile", action="migrate", id="A", name="旧槽 A", rate="176400", peq=json.dumps(legacy_peq))
        post("/api/workbench/profile", action="select", id="A")
        measurements["immediate_migration_peq"] = {"expected": legacy_peq, "slot": group("A")["peq"], "live": peq_snapshot(get()["peq"])}
        check(group("A")["peq"] == legacy_peq and peq_snapshot(get()["peq"]) == legacy_peq,
              "immediate selection of migrated active group applies the imported PEQ")
        post("/api/workbench/profile", action="select", id="C")
        post("/api/workbench/profile", action="update", id="C", backend="null", device="Null-C")
        post("/api/workbench/profile", action="select", id="D")
        post("/api/workbench/profile", action="update", id="D", backend="null", device="Null-D")
        post("/api/workbench/profile", action="select", id="C")
        check(group("C")["settings"]["device"] == "Null-C" and group("D")["settings"]["device"] == "Null-D",
              "C and D retain different output devices independently")
        post("/api/workbench/profile", action="select", id="D")
        check(get()["settings"]["device"] == "Null-D", "selecting D applies D output device")
        post("/api/workbench/profile", action="select", id="A")
        quit()
        start()
        post("/api/workbench/profile", action="select", id="A")
        check(group("A")["settings"]["rate"] == "176400" and get()["settings"]["rate"] == "176400",
              "migrated already-active group applies imported parameters without overwriting them")
        measurements["active_migration_peq"] = {"expected": legacy_peq, "slot": group("A")["peq"], "live": peq_snapshot(get()["peq"])}
        check(group("A")["peq"] == legacy_peq and peq_snapshot(get()["peq"]) == legacy_peq,
              "migrated already-active PEQ survives restart and selection without live-state overwrite")
        post("/api/workbench/profile", action="rename", id="A", name="听音配置 🎧")
        post("/api/workbench/profile", action="update", id="A", rate="48000", modorder="0")
        check(group()["name"] == "听音配置 🎧" and group()["settings"]["rate"] == "48000", "rename and automatic DSP save persist")
        phase_status, phase_result = request("/api/workbench/profile", {"action": "update", "id": "A", "phase": "intermediate"})
        check(phase_status == 200 and group()["settings"]["phase"] == "intermediate", "existing intermediate phase remains available through workbench")
        post("/api/workbench/profile", action="update", id="A", phase="")
        check(request("/api/workbench/profile", {"action": "update", "id": "B", "rate": "96000"})[0] == 409,
              "a delayed edit cannot write another active group")
        for key in ("volume", "gpuvendor"):
            check(request("/api/workbench/profile", {"action": "update", "id": "A", key: "0"})[0] == 400,
                  "group rejects " + key + " override")
        post("/api/workbench/profile", action="update", id="A", backend="null", device="Null-A")
        check(group("A")["settings"]["backend"] == "null" and group("A")["settings"]["device"] == "Null-A",
              "group accepts its own backend/device override")
        peq = get()["peq"]
        for n in range(1, 17):
            peq = {"bypass": False, "preamp": -n / 10, "bands": peq["bands"]}
            post("/api/workbench/eqpresets", action="save", id=str(n), slotId="A", name=f"Custom {n}", peq=json.dumps(peq))
        bank = get("/api/workbench/eqpresets")
        check(len([s for s in bank["slots"] if not s["empty"]]) == 16 and bank["activeId"] == "16", "sixteen independent full custom EQ snapshots save")
        check(request("/api/workbench/eqpresets", {"action": "load", "id": "1abc"})[0] == 400
              and request("/api/workbench/profile", {"action": "update", "id": "A", "eqId": "1abc"})[0] == 400,
              "EQ identifiers reject trailing nonnumeric text")
        post("/api/workbench/eqpresets", action="load", id="1", slotId="A")
        check(group()["eqId"] == "1" and abs(get()["peq"]["preamp"] + .1) < 1e-9, "saved EQ selection applies and links current group")
        stale = request("/api/workbench/eqpresets", {"action": "import", "id": "2", "slotId": "D", "productId": "test::headphone", "presetId": "test-preset"})
        check(stale[0] == 409, "stale OPRA import is rejected before changing active group")
        imported = post("/api/workbench/eqpresets", action="import", id="2", slotId="A", productId="test::headphone", presetId="test-preset")
        eq = imported["slots"][1]
        check(eq["source"] == "opra" and eq["productId"] == "test::headphone" and len(imported["peq"]["bands"]) == 2
              and abs(imported["peq"]["preamp"] + 4.2) < 1e-9 and get()["settings"]["eq"] == "",
              "cached OPRA real bands/preamp import into PEQ with provenance and no duplicate file EQ")
        db = root / "rlplayer-workbench-profiles.json"
        before = db.read_bytes()
        rejected = request("/api/workbench/eqpresets", {"action": "import", "id": "3", "productId": "bad::headphone", "presetId": "unsupported"})
        check(rejected[0] == 400 and db.read_bytes() == before and group()["eqId"] == "2", "unsupported OPRA filter rejects without partial mutation")
        post("/api/peq", preamp="-3.5")
        check(group()["peq"]["preamp"] == -3.5 and group()["eqId"] == "", "legacy PEQ hot change synchronizes current group snapshot")
        state = get()
        post("/api/workbench/apply", expected_file=state["file"], expected_revision=state["revision"], rate="96000")
        check(group()["settings"]["rate"] == "96000", "legacy workbench apply synchronizes current group")
        post("/api/settings", rate="88200")
        check(group()["settings"]["rate"] == "88200", "settings changes synchronize the same group")
        before_db, before_state = db.read_bytes(), get()
        try:
            db.chmod(stat.S_IREAD)
            failed_save = request("/api/workbench/profile", {"action": "update", "id": "A", "rate": "192000"})
        finally:
            db.chmod(stat.S_IREAD | stat.S_IWRITE)
        check(failed_save[0] == 500 and db.read_bytes() == before_db and get()["settings"] == before_state["settings"],
              "profile persistence failure rolls back the applied processing state")
        before_eq = get()["peq"]
        try:
            db.chmod(stat.S_IREAD)
            failed_eq = request("/api/workbench/eqpresets", {"action": "load", "id": "1", "slotId": "A"})
        finally:
            db.chmod(stat.S_IREAD | stat.S_IWRITE)
        check(failed_eq[0] == 500 and db.read_bytes() == before_db and get()["peq"] == before_eq,
              "EQ persistence failure rolls back PEQ and keeps saved bank unchanged")
        settings_status_storm()
        post("/api/workbench/profile", action="select", id="B")
        check(get()["settings"]["rate"] == "88200" and get()["settings"]["backend"] == "null" and get()["volume"] == -67.25,
              "group switch applies stored DSP and preserves endpoint and volume")
        post("/api/queue/set", files=str(track) + "\t3\t7\n" + str(other))
        post("/api/play", file=str(track), backend="null", device="null", volume="-67.25", rate="44100",
             filter="", shaper="none", gpuvendor="off", roomcorrauto="0", isp="0", cue_start="3", cue_end="7", queueIndex="0")
        until(lambda: get()["duration"] > 0 and get()["position"] > .1)
        state = pause()
        check(abs(state["duration"] - 4) < .01 and state["cueStart"] == 3 and state["cueEnd"] == 7, "real Null playback uses four-second CUE interval")
        volume_results=[]
        for db_value in (-120, -119.9, -.1, 0):
            reply=post('/api/volume?db='+str(db_value))
            actual=get('/api/status')['volDb']
            readback=get()['volume']
            check(abs(reply['db']-db_value)<1e-9 and abs(actual-db_value)<1e-9 and abs(readback-db_value)<1e-9,
                  f'Null live volume {db_value} dB is accepted and reads back exactly')
            volume_results.append({'requested':db_value,'response':reply['db'],'status':actual,'workbench':readback})
        measurements['volume_bounds']=volume_results
        post('/api/volume?db=-67.25')
        status = get("/api/status")
        check(status["cueStart"] == 3 and status["cueEnd"] == 7 and status["queueIndex"] == 0,
              "status polling exposes the same live CUE and queue identity")
        waveform_path = "/api/waveform?" + urllib.parse.urlencode({"file": str(track)})
        waveform = until(lambda: (lambda w: w if w.get("state") == "ready" else None)(get(waveform_path)))
        check(abs(waveform["duration"] - 4) < .0001 and 10 / 32768 - 1e-6 <= min(waveform["bins"])
              and abs(max(waveform["bins"]) - 30 / 32768) < 1e-6,
              "HTTP waveform automatically clips the current CUE and reports real channel peaks")
        explicit_path = "/api/waveform?" + urllib.parse.urlencode({"file": str(track), "clip_start": "0", "clip_length": "1"})
        explicit = until(lambda: (lambda w: w if w.get("state") == "ready" else None)(get(explicit_path)))
        check(abs(explicit["duration"] - 1) < .0001 and max(explicit["bins"]) == 0,
              "explicit read-only waveform passage is respected even while another CUE is current")
        check(request("/api/waveform?" + urllib.parse.urlencode({"file": str(track), "clip_start": "NaN"}))[0] == 400,
              "waveform route rejects non-finite passage parameters")
        check(request("/api/workbench/loop", {"enabled": "1", "file": str(track), "start": "0", "end": "4.1"})[0] == 400,
              "fragment validates relative CUE duration rather than whole file")
        post("/api/workbench/loop", enabled="1", start=".25", end=".65", repeat="1", file=str(track))
        check(request("/api/workbench/loop", {"enabled": "0", "file": str(track), "expected_revision": "stale"})[0] == 409
              and get("/api/workbench/loop")["loop"]["enabled"], "late guarded disable cannot stop the new fragment")
        post("/api/pause?on=0")
        positions = []
        deadline = time.monotonic() + 1.3
        while time.monotonic() < deadline:
            positions.append(get()["position"])
            time.sleep(.025)
        check(max(positions) <= .6501 and min(positions[3:]) >= .249 and get()["queueIndex"] == 0,
              "repeat fragment actually wraps at exact boundary without advancing CUE track")
        post("/api/workbench/loop", enabled="1", start=".3", end=".75", repeat="0", file=str(track))
        until(lambda: get("/api/status")["paused"])
        ended = get()
        check(abs(ended["position"] - .75) <= 1 / 44100 + .00001, "single-pass fragment pauses at the end sample")
        post("/api/workbench/profile", action="select", id="A")
        switched = get()
        check(switched["paused"] and switched["file"] == str(track) and switched["queueIndex"] == 0
              and abs(switched["position"] - ended["position"]) < .001 and switched["volume"] == -67.25
              and switched["cueStart"] == 3 and switched["cueEnd"] == 7,
              "switching DSP preserves real paused CUE position, queue, volume and identity")
        check(group("B")["settings"]["rate"] == "44100", "switch automatically captures the prior live processing group")
        post("/api/stop")
        post("/api/play", file=str(other), backend="null", device="null", volume="-67.25", rate="44100", filter="", gpuvendor="off", roomcorrauto="0", isp="0")
        until(lambda: get()["file"] == str(other) and get()["position"] > .1)
        check(not get("/api/workbench/loop")["loop"]["enabled"], "fragment is cleared when track identity changes")
        post("/api/stop")
        measurements.update(loop_positions=positions, cue_end_position=ended["position"], pause_preserved=switched["position"])
        expected = profiles()
        quit()
        start()
        restored = profiles()
        check(restored["active"] == expected["active"] and restored["slots"] == expected["slots"], "restart preserves all four complete groups and names")
        check(len([s for s in get("/api/workbench/eqpresets")["slots"] if not s["empty"]]) == 16,
              "restart preserves all sixteen independent EQ snapshots")
        quit()
        db.replace(root / "profiles-before-pure-peq-migration.json")
        start()
        pure_settings = group("A")["settings"]
        pure_peq = peq_snapshot(get()["peq"])
        pure_peq.update(bypass=False, preamp=-7.125)
        pure_peq["bands"][0].update(on=True, freq=1234.5, gain=-1.75, q=1.25)
        post("/api/workbench/profile", action="migrate", id="A", name="纯 PEQ 旧槽", peq=json.dumps(pure_peq))
        quit()
        start()
        post("/api/workbench/profile", action="select", id="A")
        measurements["pure_peq_migration"] = {"expected": pure_peq, "slot": group("A")["peq"], "live": peq_snapshot(get()["peq"])}
        check(group("A")["settings"] == pure_settings and group("A")["peq"] == pure_peq and peq_snapshot(get()["peq"]) == pure_peq,
              "pure PEQ-only active migration survives restart when every DSP parameter already matches")
    finally:
        quit()
        report = {"checks": checks, "passed": sum(c["ok"] for c in checks), "total": len(checks),
                  "measurements": measurements, "audio_device": "Null", "physical_device_opened": False,
                  "private_root": str(root)}
        (root / "profiles336-results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf8")
        print(json.dumps({"passed": report["passed"], "total": report["total"], "evidence": str(root)}, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
