"""Real Web/CLI PCM-dither contracts in a private Null-only install.

Run through Start-RLPlayerGuard.ps1; never uses a physical audio endpoint.
The old release is a negative control: Shaped persistence and low-rate auto
must fail, making these assertions sensitive to the reported integration bugs.
"""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import socket
import subprocess
import struct
import time
import urllib.error
import urllib.parse
import urllib.request
import wave


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("exe", type=Path)
    parser.add_argument("--cli", type=Path)
    parser.add_argument("--run-root", type=Path, required=True)
    args = parser.parse_args()
    root = args.run_root.resolve()
    root.mkdir(parents=True, exist_ok=False)
    exe = root / "rtaweb.exe"
    shutil.copy2(args.exe, exe)
    (root / "library").mkdir()
    (root / "empty").mkdir()
    (root / "library/rlplayer-library.conf").write_text(str(root / "empty") + "\n", encoding="utf8")
    (root / "rlplayer-settings.conf").write_text(
        "backend=null\ndevice=null\nvolume=-60\nrate=src\nfilter=\nshaper=none\n"
        "gpuvendor=off\nmetaon=0\nmetallm=0\nroomcorrauto=0\nisp=0\nanlmaster=0\n"
        "autoanl=0\nquickstart=0\n", encoding="utf8")
    env = dict(os.environ, RTA_NO_DEVICE="1", RTA_NET_TESTS="0", RLPLAYER_FILTER_CACHE="0",
               RLPLAYER_LIBRARY_DIR=str(root / "library"), RLPLAYER_UPDATE_DIR=str(root / "update"),
               RLPLAYER_UPDATE_MANIFEST="http://127.0.0.1:1/manifest.json", RLPLAYER_UPDATE_ALLOW_LOCAL="1")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    checks = []

    def check(ok, name, actual=None):
        checks.append({"name": name, "ok": bool(ok), "actual": actual})
        print(("PASS " if ok else "FAIL ") + name, flush=True)

    def request(path, values=None):
        data = None if values is None else urllib.parse.urlencode(values).encode()
        try:
            with urllib.request.urlopen(urllib.request.Request(base + path, data=data), timeout=12) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as response:
            try:
                body = json.load(response)
            except (ValueError, UnicodeError):
                body = {}
            return response.code, body

    def wait_status(predicate, seconds=12):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                status, body = request("/api/status")
                if status == 200 and predicate(body):
                    return body
            except (OSError, ValueError):
                pass
            time.sleep(.025)
        raise AssertionError("private Null server did not reach the expected state")

    log = (root / "server.log").open("wb")
    proc = subprocess.Popen([str(exe), "--port", str(port)], cwd=root, env=env, stdout=log, stderr=log,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    try:
        wait_status(lambda value: value.get("ok"))
        for spelling in ("Shaped", "shaped"):
            _, state = request("/api/workbench/state")
            code, applied = request("/api/workbench/apply", {
                "expected_file": state.get("file", ""), "expected_revision": state["revision"], "shaper": spelling})
            check(code == 200 and applied.get("ok"), "workbench accepts " + spelling, applied)
            _, state = request("/api/workbench/state")
            check(state.get("settings", {}).get("shaper") == "shaped", "workbench canonicalizes " + spelling,
                  state.get("settings", {}).get("shaper"))
        request("/api/settings", {"shaper": "Shaped"})
        _, saved = request("/api/settings")
        check(saved.get("kv", {}).get("shaper") == "shaped", "legacy settings canonicalizes Shaped",
              saved.get("kv", {}).get("shaper"))

        code, info = request("/api/shaper-info")
        check(code == 200 and info.get("autoResolved") == "" and info.get("sampleRate") == 0,
              "unknown output format is not presented as a measured automatic choice", info)
        if code == 200 and "options" in info:
            ids = [entry["id"] for entry in info["options"]]
            check(len(ids) == 11 and len(set(ids)) == 11 and "shaped" in ids and "Shaped" not in ids,
                  "metadata lists ten real choices plus auto exactly once", ids)
            for query in ("rate=nan", "rate=48000&rate=96000", "bits=33", "bits=-1", "rate=48000x"):
                code, _ = request("/api/shaper-info?" + query)
                check(code == 400, "metadata rejects malformed format: " + query, code)

        cases = [(44100, 16, "TPDF"), (48000, 24, "TPDF"), (96000, 24, "TPDF"),
                 (192000, 24, "NS9"), (384000, 16, "NS9"), (384000, 24, "NS9"),
                 (48000, 32, "none")]
        for rate, bits, expected in cases:
            track = root / f"silence-{rate}-{bits}.wav"
            with wave.open(str(track), "wb") as output:
                output.setparams((2, 2, rate, 0, "NONE", ""))
                output.writeframes(bytes((rate // 4) * 4))
            code, started = request("/api/play", {
                "file": str(track), "backend": "null", "volume": "-60", "dsd": "0", "rate": "src",
                "bits": bits, "filter": "", "shaper": "auto", "gpuvendor": "off", "roomcorrauto": "0",
                "isp": "0", "cleanup": "0", "loudness": "0", "rgmode": "off"})
            check(code == 200 and started.get("ok"), f"Null accepts {rate}/{bits}", started)
            done = wait_status(lambda value: value.get("current") == str(track) and not value.get("playing"))
            _, signal = request("/api/signal")
            path = signal.get("path", {})
            check(not done.get("error") and path.get("shaper") == expected,
                  f"Web auto uses actual {rate}/{bits} -> {expected}", path)
            check("Null" in path.get("device", ""), f"actual output is Null {rate}/{bits}", path.get("device"))
            code, info = request(f"/api/shaper-info?bits={bits}&rate={rate}")
            check(code == 200 and info.get("autoResolved") == expected,
                  f"metadata agrees with actual playback {rate}/{bits}", info.get("autoResolved"))
            if args.cli:
                command = [str(args.cli.resolve()), str(track), "--analyze", "--bits", str(bits), "--shaper", "auto", "--volume", "-60"]
                result = subprocess.run(command, env=env, cwd=root, capture_output=True, timeout=45)
                text = result.stdout.decode("utf8", errors="replace")
                (root / f"cli-{rate}-{bits}.log").write_bytes(result.stdout + result.stderr)
                check(result.returncode == 0 and f"PCM_SHAPER requested=auto resolved={expected} rate={rate} bits={bits}" in text,
                      f"CLI auto agrees with Web {rate}/{bits}", result.returncode)
        if args.cli:
            track = root / "silence-48000-24.wav"
            config = root / "auto.conf"
            config.write_text("shaper=auto\nbits=24\n", encoding="utf8")
            command = [str(args.cli.resolve()), str(track), "--analyze", "--config", str(config), "--shaper", "none"]
            result = subprocess.run(command, env=env, cwd=root, capture_output=True, timeout=45)
            text = result.stdout.decode("utf8", errors="replace")
            check(result.returncode == 0 and "PCM_SHAPER requested=none resolved=none" in text,
                  "explicit CLI none overrides config auto", result.returncode)

        for values in ({"dacbits": "7"}, {"dacbits": "18.5"}, {"dacbits": "nan"},
                       {"crossfeed": "2"}, {"crossfeed_freq": "299"}, {"crossfeed_level": "16"}):
            code, _ = request("/api/settings", values)
            check(code == 400, "settings rejects invalid advanced PCM value " + str(values), code)
            _, state = request("/api/workbench/state")
            code, _ = request("/api/workbench/apply", dict(values, expected_file=state["file"], expected_revision=state["revision"]))
            check(code == 400, "workbench rejects invalid advanced PCM value " + str(values), code)

        advanced = {"crossfeed": "1", "crossfeed_freq": "700", "crossfeed_level": "12", "dacbits": "20"}
        code, _ = request("/api/settings", advanced)
        _, saved = request("/api/settings")
        check(code == 200 and all(saved.get("kv", {}).get(key) == value for key, value in advanced.items()),
              "legacy settings persists crossfeed and DAC precision exactly", saved.get("kv"))
        _, state = request("/api/workbench/state")
        code, state = request("/api/workbench/apply", dict(advanced, expected_file=state["file"], expected_revision=state["revision"]))
        check(code == 200 and all(state.get("settings", {}).get(key) == value for key, value in advanced.items()),
              "workbench persists the same advanced PCM fields", state.get("settings"))
        code, reset = request("/api/workbench/apply", {"expected_file": state["file"], "expected_revision": state["revision"], "profile": "bitperfect"})
        check(code == 200 and reset.get("settings", {}).get("crossfeed") == "0" and
              reset.get("settings", {}).get("dacbits") == "0", "bitperfect preset resets crossfeed and DAC precision", reset)

        track = root / "left-only-48000.wav"
        source = [int((.0005 * math.sin(2 * math.pi * 500 * n / 48000)) * (2 ** 31)) for n in range(4800)]
        source[-1] = 1073741  # -66 dBFS pulse at EOF proves the crossfeed tail is emitted.
        with wave.open(str(track), "wb") as output:
            output.setparams((2, 4, 48000, 0, "NONE", ""))
            output.writeframes(b"".join(struct.pack("<ii", value, 0) for value in source))
        for valid_bits, expected in ((18, "TPDF"), (20, "TPDF"), (24, "TPDF")):
            code, started = request("/api/play", {
                "file": str(track), "backend": "null", "volume": "-60", "rate": "src", "bits": "32",
                "dacbits": valid_bits, "shaper": "auto", "crossfeed": "1", "crossfeed_freq": "700",
                "crossfeed_level": "12", "gpuvendor": "off", "roomcorrauto": "0", "isp": "0"})
            done = wait_status(lambda value: value.get("current") == str(track) and not value.get("playing"))
            _, signal = request("/api/signal")
            path = signal.get("path", {})
            check(code == 200 and started.get("ok") and not done.get("error") and
                  path.get("outBits") == 32 and path.get("dacBitsRequested") == valid_bits and
                  path.get("quantizationBits") == valid_bits and path.get("shaper") == expected,
                  f"actual Web reports {valid_bits} valid bits in 32-bit container", path)
            check(path.get("crossfeedRequested") is True and path.get("crossfeed") is True and
                  path.get("crossfeedFreq") == 700 and path.get("crossfeedLevelDb") == 12,
                  f"actual Web runs requested crossfeed at {valid_bits} valid bits", path)

        for valid_bits in range(8, 33):
            expected = "TPDF" if valid_bits < 16 else "none" if valid_bits == 32 else "NS9"
            code, info = request(f"/api/shaper-info?bits={valid_bits}&rate=384000")
            check(code == 200 and info.get("autoResolved") == expected,
                  f"high-rate automatic policy covers legal DAC precision {valid_bits} -> {expected}", info.get("autoResolved"))
        low_precision_track = root / "silence-384000-dac8.wav"
        with wave.open(str(low_precision_track), "wb") as output:
            output.setparams((2, 2, 384000, 0, "NONE", ""))
            output.writeframes(bytes(38400 * 4))
        code, started = request("/api/play", {
            "file": str(low_precision_track), "backend": "null", "volume": "-60", "rate": "src", "bits": "32",
            "dacbits": "8", "shaper": "auto", "crossfeed": "0", "gpuvendor": "off", "roomcorrauto": "0", "isp": "0"})
        done = wait_status(lambda value: value.get("current") == str(low_precision_track) and not value.get("playing"))
        _, signal = request("/api/signal")
        path = signal.get("path", {})
        check(code == 200 and started.get("ok") and not done.get("error") and path.get("shaper") == "TPDF" and
              path.get("quantizationBits") == 8 and path.get("outBits") == 32,
              "actual high-rate Web 8-bit DAC reduction uses TPDF without high-order saturation", path)
        if args.cli:
            result = subprocess.run([str(args.cli.resolve()), str(low_precision_track), "--analyze", "--bits", "32",
                                     "--dac-bits", "8", "--shaper", "auto", "--volume", "-60"],
                                    env=env, cwd=root, capture_output=True, timeout=45)
            text = result.stdout.decode("utf8", errors="replace")
            check(result.returncode == 0 and "PCM_SHAPER requested=auto resolved=TPDF rate=384000 bits=8" in text and
                  "containerBits=32 shaper=TPDF" in text, "actual high-rate CLI 8-bit DAC reduction uses TPDF", text)

        for mode in ("NS9", "LNS15"):
            bad = {"dsd": "0", "bits": "32", "dacbits": "8", "shaper": mode}
            code, body = request("/api/play", dict(bad, file=str(low_precision_track), backend="null"))
            check(code == 400 and not body.get("ok") and "16 bit" in body.get("error", ""),
                  f"Web rejects unsupported manual {mode}/8-bit before opening output", body)
            code, body = request("/api/settings", bad)
            check(code == 400 and not body.get("ok"), f"settings rejects manual {mode}/8-bit", body)
            _, state = request("/api/workbench/state")
            code, body = request("/api/workbench/apply", dict(bad, expected_file=state.get("file", ""), expected_revision=state["revision"]))
            check(code == 400 and not body.get("ok"), f"workbench rejects manual {mode}/8-bit", body)
            if args.cli:
                result = subprocess.run([str(args.cli.resolve()), str(low_precision_track), "--analyze", "--bits", "32",
                                         "--dac-bits", "8", "--shaper", mode], env=env, cwd=root, capture_output=True, timeout=45)
                text = result.stdout.decode("utf8", errors="replace")
                check(result.returncode == 2 and "16 bit" in text and "TPDF" in text,
                      f"CLI explicitly rejects manual {mode}/8-bit", text)

        for values in ({"crossfeed": "1"}, {"dacbits": "18"}):
            code, body = request("/api/play", dict(values, file=str(root / "raw.dsf"), backend="asio",
                                                  dsdnative="1", volume="0", isp="0", roomcorrauto="0"))
            check(not body.get("ok") and ("交叉馈送" in body.get("error", "") or "有效位深" in body.get("error", "")),
                  "raw DSD explicitly rejects " + str(values), body)
            code, body = request("/api/cast/start", dict(values, dev="http://127.0.0.1:1/unused", file=str(track)))
            check(code == 400 and not body.get("ok"), "unsupported cast explicitly rejects " + str(values), body)

        if args.cli:
            def run_cli(extra, name, source_track=track):
                result = subprocess.run([str(args.cli.resolve()), str(source_track), "--analyze"] + extra,
                                        env=env, cwd=root, capture_output=True, timeout=45)
                (root / (name + ".log")).write_bytes(result.stdout + result.stderr)
                return result, result.stdout.decode("utf8", errors="replace")

            for bits in (18, 20, 24):
                target = root / f"dac-{bits}.wav"
                result, text = run_cli(["--bits", "32", "--dac-bits", str(bits), "--shaper", "none", "--out", str(target)], f"dac-{bits}")
                if result.returncode == 0 and target.exists():
                    with wave.open(str(target), "rb") as output:
                        samples = struct.unpack("<" + "i" * (output.getnframes() * 2), output.readframes(output.getnframes()))
                    quantum = 1 << (32 - bits)
                    check(all(value % quantum == 0 for value in samples) and any(samples),
                          f"CLI {bits}-bit quantization clears exactly the 32-bit container low bits")
                    check(max(abs(samples[2*n] - source[n]) for n in range(len(source))) <= quantum / 2,
                          f"CLI {bits}-bit none rounds on the requested quantization grid")
                else:
                    check(False, f"CLI {bits}-bit export succeeded", result.returncode)
                check(result.returncode == 0 and f"bits={bits} containerBits=32 dacBitsRequested={bits}" in text,
                      f"CLI reports effective {bits} bits separately from container", text)
            result, text = run_cli(["--bits", "16", "--dac-bits", "24", "--shaper", "auto"], "dac-capped-container")
            check(result.returncode == 0 and "bits=16 containerBits=16 dacBitsRequested=24" in text,
                  "effective DAC bits cannot exceed the actual device container", text)
            config = root / "advanced.conf"
            result, text = run_cli(["--bits", "32", "--dac-bits", "20", "--crossfeed", "standard", "--save-config", str(config)], "advanced-save")
            result, text = run_cli(["--config", str(config)], "advanced-load")
            check(result.returncode == 0 and "bits=20 containerBits=32 dacBitsRequested=20" in text and
                  "CROSSFEED requested=1 cutoffHz=700.000 levelDb=6.000" in text,
                  "CLI config round-trips DAC precision and standard crossfeed", text)
            result, text = run_cli(["--config", str(config), "--crossfeed", "off", "--dac-bits", "0"], "advanced-cli-overrides")
            check(result.returncode == 0 and "dacBitsRequested=0" in text and "CROSSFEED requested=0" in text,
                  "explicit CLI off/auto overrides saved processing", text)

            rendered = {}
            for mode, chunk in (("off", 0), ("light", 0), ("light", 17), ("standard", 0)):
                target = root / f"crossfeed-{mode}-{chunk}.wav"
                result, text = run_cli(["--bits", "32", "--crossfeed", mode, "--chunk", str(chunk), "--out", str(target)], target.stem)
                check(result.returncode == 0, f"CLI crossfeed {mode} chunk={chunk} renders", text)
                if result.returncode == 0:
                    with wave.open(str(target), "rb") as output:
                        rendered[(mode, chunk)] = struct.unpack("<" + "i" * (output.getnframes() * 2), output.readframes(output.getnframes()))
            if len(rendered) == 4:
                off, light, split, standard = [rendered[key] for key in (("off", 0), ("light", 0), ("light", 17), ("standard", 0))]
                check(off == tuple(value for sample in source for value in (sample, 0)), "crossfeed disabled preserves all integer PCM samples")
                check(light == split, "crossfeed whole/chunk output and tail are identical")
                check(len(light) > len(off) and any(light[len(off):]), "crossfeed preserves finite audible-band EOF decay")
                rms = lambda values: math.sqrt(sum(value * value for value in values) / len(values))
                check(rms(standard[1:len(off):2]) > rms(light[1:len(off):2]) > 0,
                      "crossfeed adds measured opposite-channel signal and standard exceeds light")
            result, text = run_cli(["--shaper", "auto", "--bits", "32"], "transparent-auto")
            check(result.returncode == 0 and "resolved=none" in text, "auto preserves genuine integer transparency")
            result, text = run_cli(["--shaper", "auto", "--bits", "24"], "transparent-pad", root / "silence-48000-24.wav")
            check(result.returncode == 0 and "resolved=none" in text, "auto preserves lossless integer widening")
            for options in (["--dac-bits", "7"], ["--dac-bits", "18.5"], ["--crossfeed-freq", "nan"], ["--crossfeed-level", "2"]):
                result, text = run_cli(options, "invalid-" + options[0][2:] + options[1])
                check(result.returncode != 0, "CLI rejects invalid processing " + str(options), text)

            floats = [.0007 * math.sin(2 * math.pi * 997 * n / 48000) for n in range(8192) for _ in range(2)]
            data = struct.pack("<" + "f" * len(floats), *floats)
            fmt = struct.pack("<HHIIHH", 3, 2, 48000, 48000 * 8, 8, 32)
            float_track = root / "float32.wav"
            float_track.write_bytes(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
                                    struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", len(data)) + data)
            float_outputs = []
            for chunk in (0, 137):
                target = root / f"float-chunk-{chunk}.wav"
                result, text = run_cli(["--bits", "32", "--shaper", "none", "--chunk", str(chunk), "--out", str(target)], target.stem, float_track)
                check(result.returncode == 0, f"real Float32 CLI chunk={chunk} renders", text)
                if result.returncode == 0:
                    with wave.open(str(target), "rb") as output:
                        float_outputs.append(struct.unpack("<" + "i" * (output.getnframes() * 2), output.readframes(output.getnframes())))
            if len(float_outputs) == 2:
                check(float_outputs[0] == float_outputs[1], "Float32 whole and chunked CLI exports match every sample")
                rms = math.sqrt(sum((sample / 2**31 - reference)**2 for sample, reference in zip(float_outputs[1], floats)) / len(floats))
                check(rms < 3e-10, "Float32 chunked CLI preserves the source level and numeric representation", rms)
            for extra in ([], ["--dsd", "64"], ["--lowlat"], ["--native-dsd"]):
                command = [str(args.cli.resolve()), str(track), "--asio-device", "__offline_route_guard__"] + extra
                result = subprocess.run(command, env=env, cwd=root, capture_output=True, timeout=30)
                text = result.stdout.decode("utf8", errors="replace")
                check(result.returncode == 2 and "WASAPI" in text and "已拒绝播放" in text,
                      "explicit ASIO route cannot fail open to default WASAPI " + str(extra), text)
    finally:
        if proc.poll() is None:
            try:
                request("/api/quit", {})
            except (OSError, ValueError):
                pass
            proc.wait(timeout=15)
        log.close()
        report = {"checks": len(checks), "failed": sum(not item["ok"] for item in checks),
                  "audioOpened": False, "backend": "null", "results": checks}
        (root / "shaper-results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf8")
        print(json.dumps({key: report[key] for key in ("checks", "failed", "audioOpened", "backend")}), flush=True)
    return 1 if any(not item["ok"] for item in checks) else 0


if __name__ == "__main__":
    raise SystemExit(main())
