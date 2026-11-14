#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""RLPlayer macOS 增量发布工具（给 macOS 开发者使用）。

用法概览：
    python3 rlplayer_publish_mac.py doctor
    python3 rlplayer_publish_mac.py keygen --out macos-update.pem
    python3 rlplayer_publish_mac.py publish --bundle "RLPlayer.app" --version 3.2.3 \
        --endpoint https://rlplayer.rainlain.com --token "$RLPLAYER_PUBLISH_TOKEN" \
        --key macos-update.pem --cn-url https://pan.baidu.com/s/... \
        --global-url https://github.com/... --full-sha256 <64hex> --full-size 12345678
    python3 rlplayer_publish_mac.py status --endpoint ... --token ...
    python3 rlplayer_publish_mac.py verify --endpoint ... --public-key macos-update.pub.pem

只依赖 Python 3.8+ 标准库：签名用纯 Python RSA-PSS（SHA-256, saltlen=32），
不需要 pip、不需要 Homebrew、不需要 Xcode。keygen 需要系统自带 openssl。
"""
from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import json
import os
import platform
import re
import secrets
import stat
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
MAX_OBJECT = 100 * 1024 * 1024  # 服务端（nginx /admin/ 段）允许 100 MiB

# ---------------------------------------------------------------------------
# 最小 ASN.1 / RSA 实现（只用于读取未加密 PEM 私钥/公钥并做 PSS 签名）
# ---------------------------------------------------------------------------


def _der_read(data: bytes, pos: int) -> tuple[int, bytes, int]:
    if pos + 2 > len(data):
        raise ValueError("ASN.1 长度越界")
    tag = data[pos]
    pos += 1
    length = data[pos]
    pos += 1
    if length & 0x80:
        count = length & 0x7F
        length = int.from_bytes(data[pos:pos + count], "big")
        pos += count
    end = pos + length
    if end > len(data):
        raise ValueError("ASN.1 内容越界")
    return tag, data[pos:end], end


def _der_sequence(data: bytes) -> list[tuple[int, bytes]]:
    items = []
    pos = 0
    while pos < len(data):
        tag, value, pos = _der_read(data, pos)
        items.append((tag, value))
    return items


def _int_from(value: bytes) -> int:
    return int.from_bytes(value, "big")


class RSAKey:
    def __init__(self, n: int, e: int, d: int = 0, p: int = 0, q: int = 0) -> None:
        self.n, self.e, self.d, self.p, self.q = n, e, d, p, q

    @property
    def size_bytes(self) -> int:
        return (self.n.bit_length() + 7) // 8

    @property
    def kid(self) -> str:
        modulus = self.n.to_bytes(self.size_bytes, "big")
        return "rlplayer-macos-" + hashlib.sha256(modulus).hexdigest()[:16]

    def public_pem(self) -> str:
        # SubjectPublicKeyInfo { AlgorithmIdentifier rsaEncryption, BIT STRING RSAPublicKey }
        rsa_pub = _der(0x30, _der(0x02, _int_bytes(self.n)) + _der(0x02, _int_bytes(self.e)))
        alg = bytes.fromhex("300d06092a864886f70d0101010500")
        spki = _der(0x30, alg + _der(0x03, b"\x00" + rsa_pub))
        return _pem("PUBLIC KEY", spki)

    def sign_pss(self, message: bytes, salt_len: int = 32) -> bytes:
        if not self.d:
            raise ValueError("这不是私钥，无法签名")
        digest = hashlib.sha256(message).digest()
        em_bits = self.n.bit_length() - 1
        em_len = (em_bits + 7) // 8
        salt = secrets.token_bytes(salt_len)
        mprime = (b"\x00" * 8) + digest + salt
        h = hashlib.sha256(mprime).digest()
        ps_len = em_len - salt_len - len(h) - 2
        if ps_len < 0:
            raise ValueError("RSA 密钥太小，无法容纳 PSS 填充")
        em = b"\x00" * ps_len + b"\x01" + salt
        mask = _mgf1(h, em_len - len(h) - 1)
        masked_db = bytes(a ^ b for a, b in zip(em, mask))
        clear = masked_db[0] & (0xFF >> (8 * em_len - em_bits))
        encoded = bytes([clear]) + masked_db[1:] + h + b"\xbc"
        m_int = int.from_bytes(encoded, "big")
        if m_int >= self.n:
            raise ValueError("内部错误：PSS 编码超出模数")
        signature = pow(m_int, self.d, self.n)
        return signature.to_bytes(self.size_bytes, "big")

    @staticmethod
    def verify_pss(n: int, e: int, message: bytes, signature: bytes, salt_len: int = 32) -> bool:
        size = (n.bit_length() + 7) // 8
        if len(signature) != size:
            return False
        m = pow(int.from_bytes(signature, "big"), e, n)
        raw = m.to_bytes((n.bit_length() - 1 + 7) // 8, "big")
        digest = hashlib.sha256(message).digest()
        if len(raw) < salt_len + len(digest) + 2 or raw[-1] != 0xBC:
            return False
        h = raw[-1 - len(digest):-1]
        db_len = len(raw) - len(digest) - 1
        mask = _mgf1(h, db_len)
        db = bytes(a ^ b for a, b in zip(raw[:db_len], mask))
        db = bytes([db[0] & (0xFF >> (8 * len(raw) - (n.bit_length() - 1)))]) + db[1:]
        if db[0] != 0:
            return False
        index = 0
        while index < len(db) and db[index] == 0:
            index += 1
        if index >= len(db) or db[index] != 1:
            return False
        salt = db[index + 1:]
        if len(salt) != salt_len:
            return False
        return hashlib.sha256(b"\x00" * 8 + digest + salt).digest() == h


def _mgf1(seed: bytes, length: int) -> bytes:
    output = b""
    counter = 0
    while len(output) < length:
        output += hashlib.sha256(seed + counter.to_bytes(4, "big")).digest()
        counter += 1
    return output[:length]


def _int_bytes(value: int) -> bytes:
    if value == 0:
        return b"\x00"
    raw = value.to_bytes((value.bit_length() + 7) // 8, "big")
    if raw[0] & 0x80:
        raw = b"\x00" + raw
    return raw


def _der(tag: int, payload: bytes) -> bytes:
    length = len(payload)
    if length < 0x80:
        header = bytes([tag, length])
    else:
        raw = length.to_bytes((length.bit_length() + 7) // 8, "big")
        header = bytes([tag, 0x80 | len(raw)]) + raw
    return header + payload


def _pem(label: str, der: bytes) -> str:
    body = base64.b64encode(der).decode("ascii")
    lines = [body[i:i + 64] for i in range(0, len(body), 64)]
    return "-----BEGIN %s-----\n%s\n-----END %s-----\n" % (label, "\n".join(lines), label)


def load_private_key(path: str) -> RSAKey:
    text = open(path, "r", encoding="ascii", errors="replace").read()
    if "ENCRYPTED" in text:
        raise SystemExit("不支持带口令的私钥；请用 openssl genpkey -aes-256-cbc 之外的未加密私钥")
    match = re.search(r"-----BEGIN ([A-Z ]+)-----(.*?)-----END \1-----", text, re.S)
    if not match:
        raise SystemExit("文件里没有 PEM 私钥块：%s" % path)
    label, body = match.group(1), re.sub(r"\s+", "", match.group(2))
    der = base64.b64decode(body)
    tag, content, _end = _der_read(der, 0)
    if tag != 0x30:
        raise SystemExit("私钥不是 DER SEQUENCE")
    items = _der_sequence(content)
    if label == "RSA PRIVATE KEY":
        values = [_int_from(value) for item_tag, value in items if item_tag == 0x02]
        if len(values) < 9:
            raise SystemExit("PKCS#1 私钥结构异常")
        _version, n, e, d, p, q = values[:6]
        return RSAKey(n, e, d, p, q)
    if label == "PRIVATE KEY":
        inner_der = None
        for item_tag, value in items:
            if item_tag == 0x04:
                inner_der = value
                break
        if inner_der is None:
            raise SystemExit("PKCS#8 私钥里没有 RSA 数据")
        inner_tag, inner_content, _end2 = _der_read(inner_der, 0)
        inner_items = _der_sequence(inner_content)
        values = [_int_from(value) for item_tag, value in inner_items if item_tag == 0x02]
        _version, n, e, d, p, q = values[:6]
        return RSAKey(n, e, d, p, q)
    raise SystemExit("不认识的私钥类型：%s" % label)


def load_public_key(path: str) -> tuple[int, int]:
    text = open(path, "r", encoding="ascii", errors="replace").read()
    match = re.search(r"-----BEGIN ([A-Z ]+)-----(.*?)-----END \1-----", text, re.S)
    if not match:
        raise SystemExit("文件里没有 PEM 公钥块：%s" % path)
    label, body = match.group(1), re.sub(r"\s+", "", match.group(2))
    der = base64.b64decode(body)
    tag, content, _end = _der_read(der, 0)
    if tag != 0x30:
        raise SystemExit("公钥不是 DER SEQUENCE")
    items = _der_sequence(content)
    if label == "RSA PUBLIC KEY":
        values = [_int_from(value) for item_tag, value in items if item_tag == 0x02]
        return values[0], values[1]
    if label == "PUBLIC KEY":
        bit = None
        for item_tag, value in items:
            if item_tag == 0x03:
                bit = value[1:]
                break
        if bit is None:
            raise SystemExit("SPKI 里没有公钥位串")
        inner_tag, inner_content, _end2 = _der_read(bit, 0)
        values = [_int_from(value) for item_tag, value in _der_sequence(inner_content) if item_tag == 0x02]
        return values[0], values[1]
    raise SystemExit("不认识的公钥类型：%s" % label)


# ---------------------------------------------------------------------------
# 网络与对象
# ---------------------------------------------------------------------------


def http(method: str, url: str, body: bytes | None = None, headers: dict | None = None,
         timeout: int = 120) -> tuple[int, dict, bytes]:
    request = urllib.request.Request(url, data=body, method=method, headers=headers or {})
    request.add_header("User-Agent", "RLPlayer-MacPublisher/1.0")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, dict(response.headers), response.read()
    except urllib.error.HTTPError as error:
        return error.code, dict(error.headers or {}), error.read()
    except urllib.error.URLError as error:
        raise SystemExit("网络错误：%s（%s）" % (error.reason, url))


def deterministic_zip(payload: bytes) -> bytes:
    import io
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        info = zipfile.ZipInfo("payload.bin", (1980, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        archive.writestr(info, payload)
    return buffer.getvalue()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def collect(bundle: str) -> list[dict]:
    bundle = os.path.abspath(bundle)
    if not os.path.isdir(bundle):
        if bundle.endswith(".app"):
            raise SystemExit("找不到应用包：%s" % bundle)
        raise SystemExit("--bundle 必须指向 .app 目录：%s" % bundle)
    root = os.path.dirname(bundle)
    name = os.path.basename(bundle)
    files = []
    for dirpath, dirnames, filenames in os.walk(bundle, followlinks=True):
        dirnames.sort()
        for filename in sorted(filenames):
            absolute = os.path.join(dirpath, filename)
            if os.path.islink(absolute):
                absolute = os.path.realpath(absolute)
            if not os.path.isfile(absolute):
                continue
            relative = os.path.relpath(absolute, root).replace(os.sep, "/")
            mode = stat.S_IMODE(os.stat(absolute).st_mode)
            files.append({"path": relative, "source": absolute, "mode": "%04o" % mode})
    files.sort(key=lambda item: item["path"])
    if not files:
        raise SystemExit("目录里没有文件：%s" % bundle)
    return files


def iso_utc(timestamp: float) -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S.000000Z", time.gmtime(timestamp))


# ---------------------------------------------------------------------------
# 命令
# ---------------------------------------------------------------------------


def cmd_doctor(args) -> int:
    print("python      :", sys.version.split()[0], "(%s)" % platform.platform())
    found = None
    for candidate in (args.openssl, "/opt/homebrew/opt/openssl@3/bin/openssl", "/usr/local/opt/openssl@3/bin/openssl",
                      "openssl"):
        if not candidate:
            continue
        try:
            output = subprocess.run([candidate, "version"], capture_output=True, text=True, timeout=10)
            if output.returncode == 0:
                found = (candidate, output.stdout.strip() or output.stderr.strip())
                break
        except (OSError, subprocess.SubprocessError):
            continue
    print("openssl     :", (found[0] + "  " + found[1]) if found else "未找到（只有 keygen 需要；签名用纯 Python）")
    print("签名后端    : 纯 Python RSA-PSS（SHA-256, saltlen=32）")
    print("对象上限    : %.1f MiB / 个" % (MAX_OBJECT / 1048576))
    print("注意        : 私有密钥只留在你本机，任何时候都不要提交到仓库或发给别人")
    return 0


def cmd_keygen(args) -> int:
    if os.path.exists(args.out) and not args.force:
        raise SystemExit("目标已存在：%s（用 --force 覆盖）" % args.out)
    openssl = args.openssl or "openssl"
    try:
        subprocess.run([openssl, "version"], capture_output=True, check=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        raise SystemExit("找不到 openssl；macOS 自带 /usr/bin/openssl 即可生成密钥")
    result = subprocess.run([openssl, "genrsa", "-out", args.out, str(args.bits)],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit("生成密钥失败：%s" % (result.stderr.strip() or result.stdout.strip()))
    os.chmod(args.out, 0o600)
    key = load_private_key(args.out)
    public_path = args.public or (os.path.splitext(args.out)[0] + ".pub.pem")
    with open(public_path, "w", encoding="ascii") as handle:
        handle.write(key.public_pem())
    print("私钥      :", os.path.abspath(args.out), "(%d bit，权限 600)" % key.n.bit_length())
    print("公钥      :", os.path.abspath(public_path))
    print("kid       :", key.kid)
    print()
    print("下一步：把公钥和 kid 嵌进 macOS 客户端做验签；私钥留在本机，发布时用 --key 指定。")
    return 0


def build_payload(args, files: list[dict], objects: dict[str, dict]) -> dict:
    now = time.time()
    entries = []
    for item in files:
        entry = {
            "path": item["path"],
            "sha256": item["sha256"],
            "size": item["size"],
            "object": item["sha256"] + ".zip",
            "object_sha256": item["object_sha256"],
            "object_size": item["object_size"],
            "mode": item["mode"],
        }
        entries.append(entry)
    return {
        "schema": 2,
        "platform": "macos",
        "channel": "stable",
        "metadata_version": 1,
        "version": args.version,
        "published_at": iso_utc(now),
        "expires_at": iso_utc(now + args.expires_days * 86400),
        "install_mode": "incremental" if not args.manual else "manual",
        "reason": "" if not args.manual else (args.reason or "manual"),
        "notes": args.notes or "",
        "files": entries,
        "delete": [],
        "manual": {
            "cn_url": args.cn_url,
            "global_url": args.global_url,
            "extraction_code": args.extraction_code or "",
            "sha256": args.full_sha256,
            "size": args.full_size,
        },
    }


def cmd_publish(args) -> int:
    if not VERSION_RE.match(args.version):
        raise SystemExit("--version 必须是 1.2.3 形式")
    if not (args.cn_url.startswith("https://") and args.global_url.startswith("https://")):
        raise SystemExit("--cn-url / --global-url 必须是 https 链接")
    if not HEX64.match(args.full_sha256 or ""):
        raise SystemExit("--full-sha256 必须是 64 位十六进制（整包 ZIP 的 sha256）")
    token = args.token or os.environ.get("RLPLAYER_PUBLISH_TOKEN", "")
    if not token:
        raise SystemExit("缺少上传令牌：用 --token 或环境变量 RLPLAYER_PUBLISH_TOKEN")
    key = load_private_key(args.key)
    endpoint = args.endpoint.rstrip("/")
    files = collect(args.bundle)
    total = 0
    objects: dict[str, dict] = {}
    print("扫描      : %d 个文件，来自 %s" % (len(files), args.bundle))
    for index, item in enumerate(files, 1):
        digest = sha256_file(item["source"])
        item["sha256"] = digest
        item["size"] = os.path.getsize(item["source"])
        archive = deterministic_zip(open(item["source"], "rb").read())
        item["object_sha256"] = sha256_bytes(archive)
        item["object_size"] = len(archive)
        objects[digest] = {"zip": archive, "size": len(archive)}
        total += item["size"]
    print("目标大小  : %.1f MiB，去重对象 %d 个" % (total / 1048576, len(objects)))
    if args.dry_run:
        for item in files:
            print("  %s  %s  %d B" % (item["sha256"][:12], item["path"], item["size"]))
        print("dry-run：未上传")
        return 0
    uploaded = 0
    skipped = 0
    for index, (sha, entry) in enumerate(sorted(objects.items()), 1):
        if entry["size"] > MAX_OBJECT:
            raise SystemExit("对象超过服务端上限：%s (%.1f MiB)" % (sha[:12], entry["size"] / 1048576))
        url = "%s/api/v1/update/macos/objects/%s.zip" % (endpoint, sha)
        if not args.force:
            status, _headers, _body = http("HEAD", url, timeout=30)
            if status == 200:
                skipped += 1
                continue
        status, _headers, body = http("PUT", "%s/admin/api/publish/macos/objects/%s" % (endpoint, sha),
                                      body=entry["zip"],
                                      headers={"Authorization": "Bearer " + token,
                                               "Content-Type": "application/zip"})
        if status != 200:
            raise SystemExit("上传对象失败 %s：HTTP %d %s" % (sha[:12], status, body[:200].decode("utf-8", "replace")))
        uploaded += 1
        if index % 20 == 0 or index == len(objects):
            print("  对象 %d/%d（已传 %d，跳过 %d）" % (index, len(objects), uploaded, skipped))
    print("对象完成  : 上传 %d，跳过 %d" % (uploaded, skipped))
    payload = build_payload(args, files, objects)
    payload_bytes = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    signature = key.sign_pss(payload_bytes)
    envelope = {
        "alg": "PS256",
        "kid": key.kid,
        "payload": base64.urlsafe_b64encode(payload_bytes).decode("ascii").rstrip("="),
        "signature": base64.urlsafe_b64encode(signature).decode("ascii").rstrip("="),
    }
    raw = json.dumps(envelope, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if args.write_manifest:
        with open(args.write_manifest, "wb") as handle:
            handle.write(raw)
        print("清单已写  :", os.path.abspath(args.write_manifest))
    status, _headers, body = http("PUT", "%s/admin/api/publish/macos/latest" % endpoint, body=raw,
                                  headers={"Authorization": "Bearer " + token,
                                           "Content-Type": "application/json"})
    if status != 200:
        raise SystemExit("发布清单失败：HTTP %d %s" % (status, body[:300].decode("utf-8", "replace")))
    result = json.loads(body)
    print("发布成功  : v%s，%d 个文件 / %d 个对象 / %.1f MiB" %
          (result.get("version"), result.get("files", 0), result.get("objects", 0), result.get("bytes", 0) / 1048576))
    print("kid       :", result.get("kid"))
    print("校验      : %s/api/v1/update/macos/latest" % endpoint)
    return 0


def cmd_status(args) -> int:
    token = args.token or os.environ.get("RLPLAYER_PUBLISH_TOKEN", "")
    if not token:
        raise SystemExit("缺少上传令牌")
    status, _headers, body = http("GET", args.endpoint.rstrip("/") + "/admin/api/publish/macos/status",
                                  headers={"Authorization": "Bearer " + token})
    if status != 200:
        raise SystemExit("查询失败：HTTP %d %s" % (status, body[:200].decode("utf-8", "replace")))
    data = json.loads(body)
    print("最新版本  :", data["latest"]["version"] or "（尚未发布）")
    print("对象数量  :", data["objects"])
    print("存储      : %.1f MiB / %.1f MiB" % (data["storage"]["updateTree"] / 1048576,
                                               data["storage"]["updateLimit"] / 1048576))
    print("单对象上限:", "%.1f MiB" % (data["maxObjectBytes"] / 1048576))
    for release in data["releases"]:
        print("  v%-10s %s  %d 个文件  %s" % (release["version"], release["publishedAt"],
                                              release["files"], release["installMode"]))
    return 0


def cmd_verify(args) -> int:
    status, _headers, body = http("GET", args.endpoint.rstrip("/") + "/api/v1/update/macos/latest")
    if status != 200:
        raise SystemExit("取清单失败：HTTP %d" % status)
    envelope = json.loads(body)
    payload = base64.urlsafe_b64decode(envelope["payload"] + "=" * (-len(envelope["payload"]) % 4))
    signature = base64.urlsafe_b64decode(envelope["signature"] + "=" * (-len(envelope["signature"]) % 4))
    n, e = load_public_key(args.public_key)
    ok = RSAKey.verify_pss(n, e, payload, signature)
    data = json.loads(payload)
    print("版本      :", data.get("version"))
    print("平台      :", data.get("platform"), "文件", len(data.get("files") or []))
    print("签名      :", "通过" if ok else "失败")
    if not ok:
        return 2
    for item in (data.get("files") or [])[:3]:
        url = "%s/api/v1/update/macos/objects/%s" % (args.endpoint.rstrip("/"), item["object"])
        status, _headers, archive = http("GET", url)
        print("  对象 %s -> HTTP %d %d B" % (item["object"][:12], status, len(archive)))
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="RLPlayer macOS 增量发布工具")
    parser.add_argument("--openssl", default=os.environ.get("OPENSSL_BIN", ""), help="openssl 路径（仅 keygen 用）")
    sub = parser.add_subparsers(dest="command", required=True)

    doctor = sub.add_parser("doctor", help="检查运行环境")
    doctor.set_defaults(func=cmd_doctor)

    keygen = sub.add_parser("keygen", help="生成签名密钥对（私钥留在本机）")
    keygen.add_argument("--out", default="macos-update.pem")
    keygen.add_argument("--public", default="")
    keygen.add_argument("--bits", type=int, default=3072)
    keygen.add_argument("--force", action="store_true")
    keygen.set_defaults(func=cmd_keygen)

    publish = sub.add_parser("publish", help="扫描 .app、上传对象并发布签名清单")
    publish.add_argument("--bundle", required=True, help="例如 RLPlayer.app")
    publish.add_argument("--version", required=True)
    publish.add_argument("--endpoint", default="https://rlplayer.rainlain.com")
    publish.add_argument("--token", default="")
    publish.add_argument("--key", required=True, help="RSA 私钥 PEM")
    publish.add_argument("--cn-url", required=True)
    publish.add_argument("--global-url", required=True)
    publish.add_argument("--extraction-code", default="")
    publish.add_argument("--full-sha256", required=True, help="整包 ZIP 的 sha256")
    publish.add_argument("--full-size", type=int, required=True)
    publish.add_argument("--notes", default="")
    publish.add_argument("--expires-days", type=int, default=365)
    publish.add_argument("--manual", action="store_true", help="只发布手动下载入口（不提供增量对象）")
    publish.add_argument("--reason", default="")
    publish.add_argument("--dry-run", action="store_true")
    publish.add_argument("--force", action="store_true", help="已存在的对象也重新上传")
    publish.add_argument("--write-manifest", default="", help="把签名清单另存一份到本地")
    publish.set_defaults(func=cmd_publish)

    status = sub.add_parser("status", help="查看服务端配额与已发布版本")
    status.add_argument("--endpoint", default="https://rlplayer.rainlain.com")
    status.add_argument("--token", default="")
    status.set_defaults(func=cmd_status)

    verify = sub.add_parser("verify", help="下载线上清单并用公钥验签")
    verify.add_argument("--endpoint", default="https://rlplayer.rainlain.com")
    verify.add_argument("--public-key", required=True)
    verify.set_defaults(func=cmd_verify)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
