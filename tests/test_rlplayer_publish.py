from __future__ import annotations

import base64
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-server-service"
BUILD.mkdir(exist_ok=True)
sys.path.insert(0, str(ROOT / "server"))

from rlplayer_publish import b64url, canonical_json, main, public_modulus, publish_release  # noqa: E402


def decode_payload(envelope: dict) -> dict:
    return json.loads(base64.urlsafe_b64decode(envelope["payload"] + "=" * (-len(envelope["payload"]) % 4)))


class PublishTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="publish-", dir=BUILD)
        self.root = Path(self.temporary.name)
        self.release = self.root / "release"
        self.output = self.root / "data"
        self.release.mkdir()
        self.key = self.root / "external-private-key.pem"
        self.public = self.root / "public-key.pem"
        self.openssl = shutil.which("openssl")
        self.assertIsNotNone(self.openssl, "OpenSSL CLI is required for the real PS256 gate")
        self.run_openssl("genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", str(self.key))
        self.run_openssl("rsa", "-in", str(self.key), "-pubout", "-out", str(self.public))

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_openssl(self, *args: str) -> subprocess.CompletedProcess:
        result = subprocess.run([self.openssl, *args], stdin=subprocess.DEVNULL, capture_output=True, timeout=60)
        if result.returncode:
            self.fail("OpenSSL failed: " + result.stderr.decode(errors="replace"))
        return result

    def make_release(self, executable: bytes = b"RLPlayer Windows 3.2.1", webview: bytes = b"WebView runtime") -> None:
        (self.release / "RLPlayer.exe").write_bytes(executable)
        (self.release / "README.txt").write_bytes(b"same readme")
        (self.release / "obsolete.dll").write_bytes(b"remove in next version")
        runtime = self.release / "WebView2Runtime"
        runtime.mkdir(exist_ok=True)
        (runtime / "EmbeddedBrowserWebView.dll").write_bytes(webview)

    def publish(self, version: str, **kwargs):
        values = dict(github_url=f"https://github.com/example/RLPlayer-{version}-win64.zip",
                      baidu_url=f"https://pan.baidu.com/s/{version}", extract_code="test",
                      full_sha256="f" * 64, full_size=394_000_000, notes="Windows release")
        values.update(kwargs)
        return publish_release(self.release, self.output, version, self.key, openssl=self.openssl, **values)

    def verify_signature(self, envelope: dict) -> None:
        protected = b64url(canonical_json({"alg": envelope["alg"], "kid": envelope["kid"]}))
        signing_input = self.root / "signing-input.bin"
        signature = self.root / "signature.bin"
        signing_input.write_bytes((protected + "." + envelope["payload"]).encode("ascii"))
        signature.write_bytes(base64.urlsafe_b64decode(envelope["signature"] + "=" * (-len(envelope["signature"]) % 4)))
        result = self.run_openssl("dgst", "-sha256", "-verify", str(self.public),
                                  "-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:digest",
                                  "-signature", str(signature), str(signing_input))
        self.assertIn(b"Verified OK", result.stdout)
        tampered = self.root / "tampered.bin"
        tampered.write_bytes(signing_input.read_bytes() + b"x")
        failed = subprocess.run([self.openssl, "dgst", "-sha256", "-verify", str(self.public),
                                 "-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:digest",
                                 "-signature", str(signature), str(tampered)], capture_output=True, timeout=60)
        self.assertNotEqual(failed.returncode, 0)

    def test_deterministic_single_file_zip_and_real_ps256_signature(self) -> None:
        self.make_release()
        envelope, payload = self.publish("3.2.1", now=1_700_000_000)
        self.assertEqual(payload["install_mode"], "incremental")
        self.assertEqual(envelope["alg"], "PS256")
        self.assertEqual(set(envelope), {"alg", "kid", "payload", "signature"})
        self.verify_signature(envelope)
        self.assertEqual(payload["schema"], 2)
        self.assertEqual(payload["channel"], "stable")
        self.assertEqual(payload["metadata_version"], 1)
        snapshots = {}
        for entry in payload["files"]:
            object_path = self.output / "update" / "objects" / entry["object"]
            self.assertEqual(entry["object"], entry["sha256"] + ".zip")
            self.assertEqual(hashlib.sha256(object_path.read_bytes()).hexdigest(), entry["object_sha256"])
            self.assertEqual(object_path.stat().st_size, entry["object_size"])
            self.assertNotIn("delete", entry)
            with zipfile.ZipFile(object_path) as archive:
                self.assertEqual(archive.namelist(), ["payload.bin"])
                self.assertEqual(archive.getinfo("payload.bin").date_time, (1980, 1, 1, 0, 0, 0))
                raw = archive.read("payload.bin")
            self.assertEqual(hashlib.sha256(raw).hexdigest(), entry["sha256"])
            self.assertEqual(len(raw), entry["size"])
            snapshots[entry["object"]] = object_path.read_bytes()
        self.publish("3.2.1", now=1_700_000_100)
        for name, expected in snapshots.items():
            self.assertEqual((self.output / "update" / "objects" / name).read_bytes(), expected)
        latest_bytes = (self.output / "update" / "latest.json").read_bytes()
        latest = json.loads(latest_bytes)
        self.verify_signature(latest)
        public = decode_payload(latest)
        self.assertEqual(public["version"], "3.2.1")
        self.assertNotIn("source_files", public)
        self.assertNotIn("base_version", public)
        self.assertNotIn("objects_url", public)
        self.assertNotIn(self.key.read_bytes(), latest_bytes)
        self.assertFalse(any(path.suffix == ".pem" for path in self.output.rglob("*")))

    def test_cross_version_manifest_contains_changes_and_deletes(self) -> None:
        self.make_release()
        self.publish("3.2.1")
        (self.release / "RLPlayer.exe").write_bytes(b"RLPlayer Windows 3.2.2 changed")
        (self.release / "obsolete.dll").unlink()
        (self.release / "new-tool.exe").write_bytes(b"same readme")
        _, payload = self.publish("3.2.2")
        self.assertEqual(payload["base_version"], "3.2.1")
        self.assertEqual(payload["install_mode"], "incremental")
        self.assertEqual(payload["delete"], ["obsolete.dll"])
        paths = {entry["path"] for entry in payload["files"]}
        self.assertEqual(paths, {"RLPlayer.exe", "README.txt", "WebView2Runtime/EmbeddedBrowserWebView.dll", "new-tool.exe"})
        self.assertNotIn("obsolete.dll", paths)
        objects = {entry["path"]: entry["object"] for entry in payload["files"]}
        self.assertEqual(objects["README.txt"], objects["new-tool.exe"])
        for entry in payload["files"]:
            self.assertTrue((self.output / "update" / "objects" / entry["object"]).is_file())
        previous = json.loads((self.output / "update" / "releases" / "3.2.1.json").read_text())
        self.assertTrue(previous["files"])
        self.assertTrue((self.output / "update" / "latest.json").is_file())

    def test_multi_hop_delete_history_is_persistent(self) -> None:
        self.make_release()
        self.publish("3.2.1")
        (self.release / "obsolete.dll").unlink()
        self.publish("3.2.2")
        _, payload = self.publish("3.2.3")
        self.assertEqual(payload["delete"], ["obsolete.dll"])
        state = json.loads((self.output / "update" / "publisher-state.json").read_text())
        self.assertIn("obsolete.dll", state["known_paths"])
        (self.release / "obsolete.dll").write_bytes(b"temporarily restored")
        _, restored = self.publish("3.2.4")
        self.assertEqual(restored["delete"], [])
        self.assertIn("obsolete.dll", {entry["path"] for entry in restored["files"]})
        (self.release / "obsolete.dll").unlink()
        _, removed_again = self.publish("3.2.5")
        self.assertEqual(removed_again["delete"], ["obsolete.dll"])

    def test_cross_major_is_manual_without_installable_increment(self) -> None:
        self.make_release()
        self.publish("3.2.2")
        (self.release / "RLPlayer.exe").write_bytes(b"RLPlayer Windows 4.0.0")
        new_raw_sha = hashlib.sha256(b"RLPlayer Windows 4.0.0").hexdigest()
        _, payload = self.publish("4.0.0")
        self.assertEqual(payload["install_mode"], "manual")
        self.assertEqual(payload["reason"], "major-upgrade")
        self.assertEqual(payload["files"], [])
        self.assertEqual(payload["delete"], [])
        self.assertFalse((self.output / "update" / "objects" / (new_raw_sha + ".zip")).exists())
        self.assertEqual(payload["manual"]["sha256"], "f" * 64)
        self.assertEqual(payload["manual"]["size"], 394_000_000)
        self.assertTrue(payload["manual"]["cn_url"].startswith("https://pan.baidu.com/"))
        self.assertTrue(payload["manual"]["global_url"].startswith("https://github.com/"))
        self.assertEqual(payload["manual"]["extraction_code"], "test")
        self.assertFalse(any(path.name.endswith("-win64.zip") for path in self.output.rglob("*")))

    def test_webview_change_is_manual_even_within_major(self) -> None:
        self.make_release(webview=b"runtime A")
        self.publish("3.2.1")
        (self.release / "WebView2Runtime" / "EmbeddedBrowserWebView.dll").write_bytes(b"runtime B")
        _, payload = self.publish("3.2.2")
        self.assertEqual(payload["install_mode"], "manual")
        self.assertEqual(payload["reason"], "webview2-update")
        self.assertEqual(payload["files"], [])

    def test_rescue_links_and_full_metadata_are_required_for_every_release(self) -> None:
        self.make_release()
        with self.assertRaisesRegex(ValueError, "manual rescue URLs"):
            self.publish("3.2.2", baidu_url="", github_url="")
        with self.assertRaisesRegex(ValueError, "full package size"):
            self.publish("3.2.2", full_size=0)
        with self.assertRaisesRegex(ValueError, "manual rescue URLs must be HTTPS"):
            self.publish("3.2.2", baidu_url="http://pan.baidu.com/s/insecure")

    def test_three_version_retention_and_24_hour_protection(self) -> None:
        self.make_release()
        for patch in range(4):
            (self.release / "RLPlayer.exe").write_bytes(f"version 3.2.{patch}".encode())
            self.publish(f"3.2.{patch}", protection_seconds=86400)
        objects = self.output / "update" / "objects"
        first = json.loads((self.output / "update" / "releases" / "3.2.0.json").read_text())
        first_object = objects / next(item["object"] for item in first["files"] if item["path"] == "RLPlayer.exe")
        protected_time = time.time() - 23 * 3600
        for path in objects.glob("*.zip"):
            os.utime(path, (protected_time, protected_time))
        (self.release / "RLPlayer.exe").write_bytes(b"version 3.2.4")
        self.publish("3.2.4", protection_seconds=86400)
        self.assertTrue(first_object.is_file(), "an object younger than 24 h must survive cleanup")
        history = json.loads((self.output / "update" / "publisher-state.json").read_text())["history"]
        self.assertEqual([entry["version"] for entry in history], ["3.2.2", "3.2.3", "3.2.4"])
        old_time = time.time() - 25 * 3600
        os.utime(first_object, (old_time, old_time))
        (self.release / "RLPlayer.exe").write_bytes(b"version 3.2.5")
        self.publish("3.2.5", protection_seconds=86400)
        self.assertFalse(first_object.exists(), "an unreferenced object older than 24 h must be removed")
        for version in ("3.2.3", "3.2.4", "3.2.5"):
            metadata = json.loads((self.output / "update" / "releases" / (version + ".json")).read_text())
            for entry in metadata["files"]:
                self.assertTrue((objects / entry["object"]).is_file(), f"missing retained object {version}:{entry['path']}")

    def test_physical_quota_failure_does_not_replace_latest(self) -> None:
        self.make_release()
        with self.assertRaisesRegex(ValueError, "physical Windows update storage quota"):
            self.publish("3.2.2", update_storage_bytes=1)
        self.assertFalse((self.output / "update" / "latest.json").exists())
        self.assertEqual(list((self.output / "update" / "objects").glob("*.zip")), [])
        self.publish("3.2.1")
        latest_before = (self.output / "update" / "latest.json").read_bytes()
        (self.release / "RLPlayer.exe").write_bytes(b"new bytes that exceed total physical storage")
        used = sum(path.stat().st_size for path in self.output.rglob("*") if path.is_file())
        with self.assertRaisesRegex(ValueError, "physical Windows update storage quota"):
            self.publish("3.2.2", storage_bytes=used + 1)
        self.assertEqual((self.output / "update" / "latest.json").read_bytes(), latest_before)

    def test_full_package_hashed_but_never_copied_and_modulus_cli(self) -> None:
        self.make_release()
        package = self.root / "FULL-do-not-upload.zip"
        package.write_bytes(b"full package bytes used only for hashing")
        _, payload = self.publish("3.2.2", full_package=package, full_sha256="", full_size=None)
        self.assertEqual(payload["manual"]["sha256"], hashlib.sha256(package.read_bytes()).hexdigest())
        self.assertEqual(payload["manual"]["size"], package.stat().st_size)
        self.assertFalse((self.output / package.name).exists())
        modulus = public_modulus(self.key, self.openssl)
        self.assertGreaterEqual(len(modulus), 512)
        self.assertRegex(modulus, r"^[0-9A-F]+$")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = main(["--release-dir", str(self.release), "--output-dir", str(self.root / "cli-data"),
                           "--version", "3.2.2", "--private-key", str(self.key), "--openssl", self.openssl,
                           "--github-url", "https://github.com/example/full.zip",
                           "--baidu-url", "https://pan.baidu.com/s/fixture", "--full-sha256", "f" * 64,
                           "--full-size", "394000000", "--print-public-modulus"])
        self.assertEqual(result, 0)
        self.assertIn("public_modulus=" + modulus, output.getvalue())
        self.assertIn('"install_mode": "incremental"', output.getvalue())


    def test_workbench_profiles_and_older_user_state_cannot_enter_release(self) -> None:
        self.make_release()
        for relative in ("rlplayer-workbench-profiles.json", "nested/RLPLAYER-WORKBENCH-PROFILES.JSON",
                         "rlplayer-settings.conf", "nested/rlplayer-playlist.txt", "rlplayer-favs.json"):
            with self.subTest(path=relative):
                private = self.release / relative
                private.parent.mkdir(parents=True, exist_ok=True)
                private.write_bytes(b"PRIVATE-FOUR-PROFILES-SIXTEEN-EQ")
                with self.assertRaisesRegex(ValueError, "protected user state"):
                    self.publish("3.3.6")
                self.assertEqual(private.read_bytes(), b"PRIVATE-FOUR-PROFILES-SIXTEEN-EQ")
                private.unlink()

    def test_legacy_known_paths_never_schedule_user_bank_deletion(self) -> None:
        self.make_release()
        self.publish("3.3.5")
        state_path = self.output / "update" / "publisher-state.json"
        state = json.loads(state_path.read_text(encoding="utf-8"))
        state["known_paths"].extend(["rlplayer-workbench-profiles.json",
                                     "nested/RLPLAYER-WORKBENCH-PROFILES.JSON",
                                     "rlplayer-settings.conf", "nested/rlplayer-playlist.txt"])
        state_path.write_text(json.dumps(state), encoding="utf-8")
        (self.release / "obsolete.dll").unlink()
        _, payload = self.publish("3.3.6")
        self.assertEqual(payload["delete"], ["obsolete.dll"])
        self.assertFalse(any("workbench-profiles" in item["path"].lower() for item in payload["files"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
