#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tempfile
import time
import urllib.parse
import zipfile
from typing import Any

try:
    from rlplayer_service import Config, atomic_write, directory_size
except ImportError:
    from server.rlplayer_service import Config, atomic_write, directory_size

VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$")


def canonical_json(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def b64url(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def release_files(root: Path) -> list[dict[str, Any]]:
    files: list[dict[str, Any]] = []
    for path in sorted(root.rglob("*"), key=lambda item: item.as_posix()):
        if path.is_symlink():
            raise ValueError(f"symbolic links are not allowed in release: {path}")
        if not path.is_file():
            continue
        relative = PurePosixPath(path.relative_to(root).as_posix())
        if relative.is_absolute() or ".." in relative.parts or not relative.parts:
            raise ValueError(f"unsafe release path: {relative}")
        files.append({"path": relative.as_posix(), "sha256": sha256_file(path), "size": path.stat().st_size, "_source": path})
    if not files:
        raise ValueError("release directory contains no files")
    return files


def deterministic_object(source: Path, destination: Path) -> tuple[str, int]:
    temporary = destination.with_name(destination.name + ".tmp")
    try:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            info = zipfile.ZipInfo("payload.bin", (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            info.flag_bits = 0x800
            with source.open("rb") as handle:
                archive.writestr(info, handle.read(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
        with temporary.open("rb") as handle:
            data = handle.read()
        actual_sha = hashlib.sha256(data).hexdigest()
        expected_sha = destination.name.removesuffix(".zip")
        if actual_sha == expected_sha:
            raise ValueError("object digest unexpectedly equals raw source digest")
        atomic_write(destination, data)
        return actual_sha, len(data)
    finally:
        temporary.unlink(missing_ok=True)


def major(version: str) -> int:
    return int(version.split(".", 1)[0])


def webview_map(files: list[dict[str, Any]]) -> dict[str, str]:
    return {item["path"]: item["sha256"] for item in files if item["path"].startswith("WebView2Runtime/")}


def rescue_url_allowed(value: str, host: str) -> bool:
    parsed = urllib.parse.urlsplit(value)
    return parsed.scheme == "https" and parsed.hostname == host and not parsed.username and not parsed.password and not parsed.port


def _load_state(output: Path) -> dict[str, Any]:
    path = output / "publisher-state.json"
    if not path.exists():
        return {"history": []}
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict) or not isinstance(value.get("history"), list):
        raise ValueError("invalid publisher state")
    return value


def _load_previous(output: Path, state: dict[str, Any]) -> dict[str, Any] | None:
    if not state["history"]:
        return None
    version = state["history"][-1]["version"]
    path = output / "releases" / (version + ".json")
    if not path.is_file():
        raise ValueError("previous release metadata is missing")
    return json.loads(path.read_text(encoding="utf-8"))


def sign_envelope(payload: dict[str, Any], private_key: Path, kid: str, openssl: str,
                  work_dir: Path) -> tuple[dict[str, Any], bytes]:
    if not private_key.is_file():
        raise ValueError("OpenSSL private key file does not exist")
    protected = b64url(canonical_json({"alg": "PS256", "kid": kid}))
    encoded_payload = b64url(canonical_json(payload))
    signing_input = (protected + "." + encoded_payload).encode("ascii")
    with tempfile.TemporaryDirectory(prefix=".sign-", dir=work_dir) as directory:
        input_path = Path(directory) / "input.bin"
        signature_path = Path(directory) / "signature.bin"
        input_path.write_bytes(signing_input)
        command = [
            openssl, "dgst", "-sha256", "-sign", str(private_key),
            "-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:digest",
            "-out", str(signature_path), str(input_path),
        ]
        result = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, timeout=60, check=False)
        if result.returncode != 0 or not signature_path.is_file():
            raise RuntimeError("OpenSSL PS256 signing failed: " + result.stderr.decode("utf-8", errors="replace")[-1000:])
        signature = signature_path.read_bytes()
    envelope = {"alg": "PS256", "kid": kid, "payload": encoded_payload, "signature": b64url(signature)}
    return envelope, canonical_json(envelope) + b"\n"


def public_modulus(private_key: Path, openssl: str) -> str:
    result = subprocess.run([openssl, "rsa", "-in", str(private_key), "-noout", "-modulus"],
                            stdin=subprocess.DEVNULL, capture_output=True, timeout=60, check=False)
    if result.returncode != 0:
        raise RuntimeError("OpenSSL public modulus export failed: " + result.stderr.decode("utf-8", errors="replace")[-1000:])
    text = result.stdout.decode("ascii", errors="strict").strip()
    if not text.startswith("Modulus=") or not re.fullmatch(r"Modulus=[0-9A-Fa-f]+", text):
        raise RuntimeError("unexpected OpenSSL modulus output")
    return text.split("=", 1)[1].upper()


def publish_release(
    release_dir: str | os.PathLike[str], output_dir: str | os.PathLike[str], version: str,
    private_key: str | os.PathLike[str], *, openssl: str = "openssl", kid: str | None = None,
    full_package: str | os.PathLike[str] | None = None, full_sha256: str = "", full_size: int | None = None,
    baidu_url: str = "", github_url: str = "", extract_code: str = "", notes: str = "",
    storage_bytes: int = Config.storage_bytes, update_storage_bytes: int = Config.windows_update_bytes,
    keep_versions: int = 3, protection_seconds: int = 86400, now: float | None = None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    if not VERSION_RE.fullmatch(version):
        raise ValueError("version must be a safe semantic version such as 3.2.2")
    if keep_versions < 1 or protection_seconds < 0:
        raise ValueError("invalid retention configuration")
    root = Path(release_dir).resolve()
    output = Path(output_dir).resolve()
    if not root.is_dir():
        raise ValueError("release directory does not exist")
    output.mkdir(parents=True, exist_ok=True)
    objects = output / "update" / "objects"
    releases = output / "update" / "releases"
    objects.mkdir(parents=True, exist_ok=True)
    releases.mkdir(parents=True, exist_ok=True)
    state = _load_state(output / "update")
    previous = _load_previous(output / "update", state)
    current = release_files(root)
    old_files = previous.get("source_files", []) if previous else []
    old_by_path = {item["path"]: item for item in old_files}
    current_paths = {item["path"] for item in current}
    state_paths = state.get("known_paths", [])
    if not isinstance(state_paths, list) or any(not isinstance(path, str) for path in state_paths):
        raise ValueError("invalid publisher known_paths")
    known_paths = set(state_paths)
    if any(not path or "\\" in path or PurePosixPath(path).is_absolute() or ".." in PurePosixPath(path).parts
           for path in known_paths):
        raise ValueError("unsafe publisher known_paths")
    known_paths.update(old_by_path)
    deleted = sorted(known_paths - current_paths)
    reasons: list[str] = []
    if previous and major(str(previous["version"])) != major(version):
        reasons.append("major-upgrade")
    if previous and webview_map(old_files) != webview_map(current):
        reasons.append("webview2-update")
    manual = bool(reasons)
    timestamp = time.time() if now is None else now
    if not baidu_url or not github_url:
        raise ValueError("every release requires both Baidu and GitHub manual rescue URLs")
    if not rescue_url_allowed(baidu_url, "pan.baidu.com") or not rescue_url_allowed(github_url, "github.com"):
        raise ValueError("manual rescue URLs must be HTTPS on pan.baidu.com and github.com without credentials or ports")
    manual_info = {
        "cn_url": baidu_url, "global_url": github_url, "extraction_code": extract_code,
        "sha256": full_sha256.lower(), "size": full_size or 0,
    }
    if full_package:
        package = Path(full_package).resolve()
        if not package.is_file():
            raise ValueError("full package does not exist")
        manual_info["sha256"] = sha256_file(package)
        manual_info["size"] = package.stat().st_size
    if not re.fullmatch(r"[0-9a-f]{64}", manual_info["sha256"]):
        raise ValueError("full_sha256 must be 64 lowercase hexadecimal characters")
    if full_size is not None and full_size < 0:
        raise ValueError("full_size cannot be negative")
    if manual_info["size"] <= 0:
        raise ValueError("manual full package size must be positive")
    payload: dict[str, Any] = {
        "schema": 2, "platform": "windows", "channel": "stable", "metadata_version": 1,
        "version": version,
        "published_at": dt.datetime.fromtimestamp(timestamp, dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "expires_at": dt.datetime.fromtimestamp(timestamp + 366 * 86400, dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "install_mode": "manual" if manual else "incremental",
        "reason": ",".join(reasons), "files": [], "delete": [] if manual else deleted,
        "manual": manual_info, "notes": notes,
    }
    new_objects: list[tuple[Path, int]] = []
    if not manual:
        manifest: list[dict[str, Any]] = []
        for item in current:
            object_name = item["sha256"] + ".zip"
            destination = objects / object_name
            if destination.is_file():
                object_sha, object_size = sha256_file(destination), destination.stat().st_size
                with zipfile.ZipFile(destination) as archive:
                    if archive.namelist() != ["payload.bin"]:
                        raise ValueError(f"existing object has wrong member: {object_name}")
                    if hashlib.sha256(archive.read("payload.bin")).hexdigest() != item["sha256"]:
                        raise ValueError(f"existing object has wrong raw content: {object_name}")
            else:
                object_sha, object_size = deterministic_object(item["_source"], destination)
                new_objects.append((destination, object_size))
            manifest.append({
                "path": item["path"], "sha256": item["sha256"], "size": item["size"],
                "object": object_name, "object_sha256": object_sha, "object_size": object_size,
            })
            physical = directory_size(output / "update")
            if physical > update_storage_bytes or directory_size(output) > storage_bytes:
                for created, _ in new_objects:
                    created.unlink(missing_ok=True)
                raise ValueError("physical Windows update storage quota exceeded; latest was not changed")
        payload["files"] = manifest
    source_files = [{key: item[key] for key in ("path", "sha256", "size")} for item in current]
    payload["source_files"] = source_files
    payload["base_version"] = previous["version"] if previous else ""
    signed_payload = {key: value for key, value in payload.items() if key not in ("base_version", "source_files")}
    key_path = Path(private_key).resolve()
    key_id = kid or "rlplayer-win-" + hashlib.sha256(bytes.fromhex(public_modulus(key_path, openssl))).hexdigest()[:16]
    envelope, encoded = sign_envelope(signed_payload, key_path, key_id, openssl, output)
    history = [entry for entry in state["history"] if entry.get("version") != version]
    history.append({"version": version, "published_at": payload["published_at"]})
    history = history[-keep_versions:]
    retained = {entry["version"] for entry in history}
    referenced: set[str] = set()
    for retained_version in retained:
        metadata = releases / (retained_version + ".json")
        if metadata.is_file() and retained_version != version:
            old_payload = json.loads(metadata.read_text(encoding="utf-8"))
            referenced.update(item["object"] for item in old_payload.get("files", []))
    referenced.update(item["object"] for item in payload.get("files", []))
    release_data = canonical_json(payload) + b"\n"
    state_data = canonical_json({"history": history, "known_paths": sorted(known_paths | current_paths)}) + b"\n"
    for metadata in releases.glob("*.json"):
        if metadata.stem not in retained and metadata.stem != version:
            age = timestamp - metadata.stat().st_mtime
            if age >= protection_seconds:
                metadata.unlink()
    for candidate in objects.glob("*.zip"):
        if candidate.name not in referenced and timestamp - candidate.stat().st_mtime >= protection_seconds:
            candidate.unlink()
    metadata_peak = len(release_data) + len(encoded) + len(state_data)
    physical = directory_size(output / "update") + metadata_peak
    if physical > update_storage_bytes or directory_size(output) + metadata_peak > storage_bytes:
        for created, _ in new_objects:
            created.unlink(missing_ok=True)
        raise ValueError("physical Windows update storage quota exceeded; latest was not changed")
    atomic_write(releases / (version + ".json"), release_data)
    atomic_write(output / "update" / "latest.json", encoded)
    atomic_write(output / "update" / "publisher-state.json", state_data)
    return envelope, payload


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Publish deterministic RLPlayer Windows incremental objects without uploading anything")
    parser.add_argument("--release-dir", required=True, help="unpacked FULL release directory; files are read only")
    parser.add_argument("--output-dir", required=True, help="service data directory")
    parser.add_argument("--version", required=True)
    parser.add_argument("--private-key", required=True, help="external RSA private key; never copied to output or repository")
    parser.add_argument("--openssl", default="openssl")
    parser.add_argument("--kid")
    parser.add_argument("--full-package", help="local FULL package used only to calculate SHA-256 and size; never copied")
    parser.add_argument("--full-sha256", default="")
    parser.add_argument("--full-size", type=int)
    parser.add_argument("--baidu-url", default="")
    parser.add_argument("--github-url", default="")
    parser.add_argument("--extract-code", default="")
    parser.add_argument("--notes", default="")
    parser.add_argument("--storage-bytes", type=int, default=Config.storage_bytes)
    parser.add_argument("--update-storage-bytes", type=int, default=Config.windows_update_bytes)
    parser.add_argument("--print-public-modulus", action="store_true")
    args = parser.parse_args(argv)
    if args.print_public_modulus:
        print("public_modulus=" + public_modulus(Path(args.private_key).resolve(), args.openssl))
    envelope, payload = publish_release(
        args.release_dir, args.output_dir, args.version, args.private_key,
        openssl=args.openssl, kid=args.kid, full_package=args.full_package,
        full_sha256=args.full_sha256, full_size=args.full_size, baidu_url=args.baidu_url,
        github_url=args.github_url, extract_code=args.extract_code, notes=args.notes,
        storage_bytes=args.storage_bytes, update_storage_bytes=args.update_storage_bytes,
    )
    print(json.dumps({"ok": True, "version": payload["version"], "install_mode": payload["install_mode"],
                      "files": len(payload["files"]), "deleted": len(payload["delete"]),
                      "latest": str(Path(args.output_dir).resolve() / "update" / "latest.json"),
                      "alg": envelope["alg"], "kid": envelope["kid"]}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
