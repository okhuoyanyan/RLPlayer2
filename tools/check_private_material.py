import argparse
import json
import pathlib
import sys
import zipfile

PRIVATE_MARKERS = (
    b"-----BEGIN PRIVATE KEY-----",
    b"-----BEGIN RSA PRIVATE KEY-----",
    b"-----BEGIN OPENSSH PRIVATE KEY-----",
    b"-----BEGIN ENCRYPTED PRIVATE KEY-----",
    b"-----BEGIN EC PRIVATE KEY-----",
)
SECRET_FIELDS = (b'"totp_secret"', b'"admin_password_hash"', b'"publisher_private_key"')
SENSITIVE_SUFFIXES = {".pem", ".key", ".pfx", ".p12", ".ppk"}
MAX_PROBE = 2 * 1024 * 1024


def finding(path, reason):
    return {"path": path.replace("\\", "/"), "reason": reason}


def inspect(name, read):
    hits = []
    lower = name.lower()
    suffix = pathlib.PurePosixPath(lower).suffix
    base = pathlib.PurePosixPath(lower).name
    if suffix in SENSITIVE_SUFFIXES:
        hits.append(finding(name, "private-key file extension"))
    if base in {"id_rsa", "id_ed25519", "windows-update-private.pem", "publisher-auth.json"}:
        hits.append(finding(name, "credential filename"))
    try:
        data = read(MAX_PROBE + 1)
    except Exception as exc:
        return [finding(name, "unreadable: " + type(exc).__name__)]
    if len(data) > MAX_PROBE:
        data = data[:MAX_PROBE]
    for marker in PRIVATE_MARKERS + SECRET_FIELDS:
        if marker in data:
            hits.append(finding(name, "private material marker: " + marker.decode("ascii")))
            break
    return hits


def read_file_bounded(path, limit):
    with path.open("rb") as stream:
        return stream.read(limit)


def scan(path):
    hits = []
    if path.is_file() and zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            infos = archive.infolist()
            if len(infos) > 100000:
                return [finding(str(path), "too many archive entries")]
            for info in infos:
                if info.is_dir():
                    continue
                def read_entry(limit, i=info):
                    with archive.open(i) as stream:
                        return stream.read(limit)
                hits.extend(inspect(info.filename, read_entry))
        return hits
    if path.is_file():
        return inspect(path.name, lambda limit: read_file_bounded(path, limit))
    if not path.is_dir():
        return [finding(str(path), "path does not exist")]
    files = [p for p in path.rglob("*") if p.is_file()]
    if len(files) > 100000:
        return [finding(str(path), "too many files")]
    for item in files:
        rel = item.relative_to(path).as_posix()
        hits.extend(inspect(rel, lambda limit, p=item: read_file_bounded(p, limit)))
    return hits


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=pathlib.Path)
    args = parser.parse_args()
    hits = scan(args.path)
    print(json.dumps({"ok": not hits, "path": str(args.path), "hits": hits}, ensure_ascii=False, indent=2))
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
