import json
import pathlib
import subprocess
import sys
import tempfile
import zipfile

root = pathlib.Path(__file__).resolve().parents[1]
script = root / "tools/check_private_material.py"
checks = 0


def run(path, expected, label):
    global checks
    result = subprocess.run([sys.executable, str(script), str(path)], capture_output=True, text=True)
    report = json.loads(result.stdout)
    assert (result.returncode == 0) is expected and report["ok"] is expected, (label, result.stdout, result.stderr)
    checks += 1
    print("PASS", label)


with tempfile.TemporaryDirectory(prefix="rlplayer-private-gate-") as tmp:
    root_dir = pathlib.Path(tmp)
    (root_dir / "public.txt").write_text("-----BEGIN PUBLIC KEY-----\nfixture\n", encoding="ascii")
    run(root_dir, True, "public keys and ordinary release files are allowed")
    (root_dir / "bad.pem").write_text("not even a key", encoding="ascii")
    run(root_dir, False, "private-key extensions are rejected")
    (root_dir / "bad.pem").unlink()
    (root_dir / "notes.txt").write_text("-----BEGIN OPENSSH PRIVATE KEY-----\nfixture", encoding="ascii")
    run(root_dir, False, "embedded private-key markers are rejected")
    (root_dir / "notes.txt").unlink()
    (root_dir / "server.json").write_text('{"totp_secret":"fixture"}', encoding="ascii")
    run(root_dir, False, "dashboard secrets are rejected")
    (root_dir / "server.json").unlink()
    archive = root_dir / "release.zip"
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr("public.txt", "ordinary release")
    run(archive, True, "clean ZIP passes")
    with zipfile.ZipFile(archive, "a") as z:
        z.writestr("tools/publisher-auth.json", "{}")
    run(archive, False, "credential filenames inside ZIP are rejected")
print(json.dumps({"checks": checks, "failures": 0}))
