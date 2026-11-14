#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import csv
import datetime as dt
import hashlib
import hmac
import html
import io
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import sqlite3
import struct
import threading
import time
import urllib.parse
import uuid
import zipfile
from dataclasses import dataclass, fields
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

import admin_ui
import geoip

CN_TZ = dt.timezone(dt.timedelta(hours=8), "Asia/Shanghai")
MAX_REQUEST = 12 * 1024 * 1024
MAX_TEXT = 16 * 1024
MAX_DIAG = 8 * 1024 * 1024
OBJECT_RE = re.compile(r"^[0-9a-f]{64}$")
MACOS_VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$")
USERNAME_RE = re.compile(r"[A-Za-z0-9_.-]{1,32}")
IDEMPOTENCY_RE = re.compile(r"^[A-Za-z0-9._:-]{8,160}$")
SECRET_RE = re.compile(
    r"(?i)(bearer\s+)[A-Za-z0-9._~+/=-]{8,}|"
    r"((?:password|passwd|pwd|api[_-]?key|access[_-]?token|refresh[_-]?token|secret|authorization)"
    r"(?:\\?\"|')?\s*[=:]\s*(?:\\?\"|'|%22)?)[^\s,;\\}\"'&]+"
)
EMAIL_RE = re.compile(r"(?i)\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b")
SECRET_BYTES_RE = re.compile(
    rb"(?i)(bearer\s+)[A-Za-z0-9._~+/=-]{8,}|"
    rb"((?:password|passwd|pwd|api[_-]?key|access[_-]?token|refresh[_-]?token|secret|authorization)"
    rb"(?:\\?\"|')?\s*[=:]\s*(?:\\?\"|'|%22)?)[^\s,;\\}\"'&]+"
)
EMAIL_BYTES_RE = re.compile(rb"(?i)\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b")


def utc_iso(ts: float | None = None) -> str:
    return dt.datetime.fromtimestamp(time.time() if ts is None else ts, dt.timezone.utc).isoformat().replace("+00:00", "Z")


def month_for(ts: float | None = None) -> str:
    return dt.datetime.fromtimestamp(time.time() if ts is None else ts, CN_TZ).strftime("%Y-%m")


def day_for(ts: float | None = None) -> str:
    return dt.datetime.fromtimestamp(time.time() if ts is None else ts, CN_TZ).strftime("%Y-%m-%d")


def seconds_to_next_month(ts: float | None = None) -> int:
    now = dt.datetime.fromtimestamp(time.time() if ts is None else ts, CN_TZ)
    year, month = (now.year + 1, 1) if now.month == 12 else (now.year, now.month + 1)
    following = dt.datetime(year, month, 1, tzinfo=CN_TZ)
    return max(1, int((following - now).total_seconds()))


def sanitize_text(value: str) -> str:
    first = SECRET_RE.sub(lambda m: (m.group(1) or m.group(2) or "") + "[REDACTED]", value)
    first = EMAIL_RE.sub("[EMAIL_REDACTED]", first)
    return SECRET_RE.sub(lambda m: (m.group(1) or m.group(2) or "") + "[REDACTED]", first)


def sanitize_diag(value: bytes) -> bytes:
    replace = lambda match: (match.group(1) or match.group(2) or b"") + b"[REDACTED]"
    first = SECRET_BYTES_RE.sub(replace, value)
    first = EMAIL_BYTES_RE.sub(b"[EMAIL_REDACTED]", first)
    return SECRET_BYTES_RE.sub(replace, first)


def mask_ip(value: str) -> str:
    try:
        address = ipaddress.ip_address(value)
        if address.version == 4:
            parts = value.split(".")
            return ".".join(parts[:3]) + ".*"
        return ":".join(address.exploded.split(":")[:3]) + ":*:*"
    except ValueError:
        return "hidden"


def safe_client(value: Any) -> str:
    return value.strip()[:160] if isinstance(value, str) else ""


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp-" + secrets.token_hex(8))
    try:
        with temporary.open("xb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def directory_size(root: Path) -> int:
    total = 0
    if not root.exists():
        return 0
    for path in root.rglob("*"):
        try:
            if path.is_file() and not path.is_symlink():
                total += path.stat().st_size
        except FileNotFoundError:
            pass
    return total


@dataclass
class Config:
    storage_bytes: int = 5 * 1024**3
    update_storage_bytes: int = int(4.5 * 1024**3)
    feedback_storage_bytes: int = 512 * 1024**2
    windows_update_bytes: int = 5 * 1024**3
    macos_update_bytes: int = 5 * 1024**3
    monthly_object_bytes: int = 245_000_000_000
    monthly_api_bytes: int = 5_000_000_000
    feedback_rate: int = 5
    feedback_rate_window: int = 60
    login_rate: int = 5
    login_rate_window: int = 300
    reservation_ttl: int = 300
    session_ttl: int = 3600
    retention_days: int = 90
    x_accel: bool = False
    x_accel_prefix: str = "/objects/"
    secure_cookie: bool = False
    accounting_token: str = ""
    geo_db: str = ""
    blocked_retry_seconds: int = 86400
    publish_token_macos: str = ""
    pbkdf2_iterations: int = 600_000

    @property
    def monthly_total_bytes(self) -> int:
        # Legacy 245 GB + 5 GB fields now describe one shared monthly budget.
        # Retain config compatibility; storage quotas remain independent.
        return self.monthly_object_bytes + self.monthly_api_bytes

    @classmethod
    def load(cls, values: dict[str, Any] | None = None) -> "Config":
        values = dict(values or {})
        env_token = os.environ.get("RLPLAYER_ACCOUNTING_TOKEN")
        if env_token and not values.get("accounting_token"):
            values["accounting_token"] = env_token
        mac_token = os.environ.get("RLPLAYER_PUBLISH_TOKEN_MACOS")
        if mac_token and not values.get("publish_token_macos"):
            values["publish_token_macos"] = mac_token
        allowed = {item.name for item in fields(cls)}
        unknown = set(values) - allowed
        if unknown:
            raise ValueError("unknown config keys: " + ", ".join(sorted(unknown)))
        config = cls(**{key: value for key, value in values.items() if key in allowed})
        for name in (
            "storage_bytes", "update_storage_bytes", "feedback_storage_bytes",
            "monthly_object_bytes", "monthly_api_bytes", "reservation_ttl",
            "session_ttl", "retention_days", "feedback_rate", "login_rate",
            "blocked_retry_seconds", "pbkdf2_iterations",
            "windows_update_bytes", "macos_update_bytes",
        ):
            if int(getattr(config, name)) <= 0:
                raise ValueError(f"{name} must be positive")
        if config.update_storage_bytes + config.feedback_storage_bytes > config.storage_bytes:
            raise ValueError("update and feedback storage quotas exceed total storage")
        if not config.x_accel_prefix.startswith("/") or not config.x_accel_prefix.endswith("/"):
            raise ValueError("x_accel_prefix must start and end with /")
        return config


class HTTPFailure(Exception):
    def __init__(self, status: int, message: str, code: str = "bad_request", retry: int | None = None):
        super().__init__(message)
        self.status = status
        self.message = message
        self.code = code
        self.retry = retry


class RateLimiter:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._hits: dict[tuple[str, str], list[float]] = {}

    def check(self, bucket: str, identity: str, limit: int, window: int) -> int:
        now = time.monotonic()
        key = (bucket, identity)
        with self._lock:
            hits = [stamp for stamp in self._hits.get(key, []) if now - stamp < window]
            if len(hits) >= limit:
                self._hits[key] = hits
                return max(1, int(window - (now - hits[0])))
            hits.append(now)
            self._hits[key] = hits
            if len(self._hits) > 4096:
                self._hits = {k: v for k, v in self._hits.items() if v and now - v[-1] < window}
            return 0


class ClosingConnection(sqlite3.Connection):
    def __exit__(self, exc_type: Any, exc_value: Any, traceback: Any) -> bool:
        try:
            super().__exit__(exc_type, exc_value, traceback)
        finally:
            self.close()
        return False


class Store:
    def __init__(self, root: str | os.PathLike[str], config: Config | None = None):
        self.root = Path(root).resolve()
        self.config = config or Config()
        self.db_path = self.root / "private" / "service.sqlite3"
        self.objects = self.root / "update" / "objects"
        self.latest = self.root / "update" / "latest.json"
        self.feedback_dir = self.root / "private" / "feedback"
        self.feedback_dir.mkdir(parents=True, exist_ok=True)
        self.objects.mkdir(parents=True, exist_ok=True)
        self.mac_root = self.root / "update" / "macos"
        self.mac_objects = self.mac_root / "objects"
        self.mac_latest = self.mac_root / "latest.json"
        self.mac_releases = self.mac_root / "releases"
        self.mac_objects.mkdir(parents=True, exist_ok=True)
        self.mac_releases.mkdir(parents=True, exist_ok=True)
        self.geo = geoip.GeoLookup(self.config.geo_db)
        self._initialize()

    def connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.db_path, timeout=15, isolation_level=None, factory=ClosingConnection)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA busy_timeout=15000")
        connection.execute("PRAGMA foreign_keys=ON")
        return connection

    def _initialize(self) -> None:
        self.db_path.parent.mkdir(parents=True, exist_ok=True)
        with self.connect() as db:
            db.execute("PRAGMA journal_mode=WAL")
            db.execute("PRAGMA synchronous=NORMAL")
            db.executescript(
                """
                CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS feedback (
                    id TEXT PRIMARY KEY, idem TEXT UNIQUE NOT NULL, ts REAL NOT NULL,
                    text TEXT NOT NULL, version TEXT NOT NULL, os TEXT NOT NULL,
                    client TEXT NOT NULL, diag_path TEXT, diag_size INTEGER NOT NULL DEFAULT 0
                );
                CREATE TABLE IF NOT EXISTS events (
                    id INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL NOT NULL, day TEXT NOT NULL,
                    kind TEXT NOT NULL, ip TEXT NOT NULL, client TEXT NOT NULL,
                    version TEXT NOT NULL, path TEXT NOT NULL, status INTEGER NOT NULL,
                    bytes INTEGER NOT NULL DEFAULT 0, detail TEXT NOT NULL DEFAULT '{}'
                );
                CREATE INDEX IF NOT EXISTS events_ts ON events(ts);
                CREATE INDEX IF NOT EXISTS events_ip ON events(ip);
                CREATE TABLE IF NOT EXISTS daily (
                    day TEXT NOT NULL, kind TEXT NOT NULL, requests INTEGER NOT NULL,
                    bytes INTEGER NOT NULL, errors INTEGER NOT NULL, PRIMARY KEY(day, kind)
                );
                CREATE TABLE IF NOT EXISTS traffic (
                    month TEXT PRIMARY KEY, object_bytes INTEGER NOT NULL DEFAULT 0,
                    api_bytes INTEGER NOT NULL DEFAULT 0
                );
                CREATE TABLE IF NOT EXISTS reservations (
                    id TEXT PRIMARY KEY, month TEXT NOT NULL, object TEXT NOT NULL,
                    expected INTEGER NOT NULL, actual INTEGER, state TEXT NOT NULL,
                    created REAL NOT NULL
                );
                CREATE INDEX IF NOT EXISTS reservations_state ON reservations(state, created);
                CREATE TABLE IF NOT EXISTS sessions (
                    token_hash TEXT PRIMARY KEY, csrf TEXT NOT NULL, expires REAL NOT NULL
                );
                CREATE TABLE IF NOT EXISTS reports (
                    id INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL NOT NULL,
                    from_version TEXT NOT NULL, to_version TEXT NOT NULL, ok INTEGER NOT NULL,
                    ms INTEGER NOT NULL, client TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS users (
                    username TEXT PRIMARY KEY, salt TEXT NOT NULL, hash TEXT NOT NULL,
                    iterations INTEGER NOT NULL, role TEXT NOT NULL DEFAULT 'admin', created REAL NOT NULL
                );
                CREATE TABLE IF NOT EXISTS blacklist (
                    id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL, value TEXT NOT NULL,
                    note TEXT NOT NULL DEFAULT '', created REAL NOT NULL, created_by TEXT NOT NULL DEFAULT '',
                    active INTEGER NOT NULL DEFAULT 1, UNIQUE(kind,value)
                );
                CREATE TABLE IF NOT EXISTS traffic_platform (
                    month TEXT NOT NULL, platform TEXT NOT NULL,
                    object_bytes INTEGER NOT NULL DEFAULT 0, api_bytes INTEGER NOT NULL DEFAULT 0,
                    PRIMARY KEY(month,platform)
                );
                """
            )
            # 增量迁移：老库补列（SQLite 不支持 IF NOT EXISTS 加列）
            for table, column, declaration in (
                ("events", "platform", "TEXT NOT NULL DEFAULT ''"),
                ("feedback", "platform", "TEXT NOT NULL DEFAULT ''"),
                ("sessions", "username", "TEXT NOT NULL DEFAULT ''"),
                ("reservations", "platform", "TEXT NOT NULL DEFAULT 'windows'"),
            ):
                names = {row["name"] for row in db.execute("PRAGMA table_info(%s)" % table)}
                if column not in names:
                    db.execute("ALTER TABLE %s ADD COLUMN %s %s" % (table, column, declaration))
            self._backfill_platform(db)
            db.execute("UPDATE feedback SET platform='mac' WHERE platform='' "
                       "AND (lower(os) LIKE '%mac%' OR lower(os) LIKE '%darwin%')")
            db.execute("UPDATE feedback SET platform='windows' WHERE platform=''")
            if db.execute("SELECT 1 FROM meta WHERE k='hmac_secret'").fetchone() is None:
                db.execute("INSERT INTO meta(k,v) VALUES('hmac_secret',?)", (secrets.token_hex(32),))

    def _backfill_platform(self, db: sqlite3.Connection) -> None:
        """老库首次升级：把已有对象/API 流量按 windows 平台补进分平台表。"""
        if db.execute("SELECT COUNT(*) AS n FROM traffic_platform").fetchone()["n"]:
            return
        rows = db.execute("SELECT day,kind,bytes FROM events WHERE kind IN ('object_download','api')").fetchall()
        if not rows:
            return
        aggregate: dict[str, list[int]] = {}
        for row in rows:
            month = str(row["day"])[:7]
            slot = aggregate.setdefault(month, [0, 0])
            if row["kind"] == "object_download":
                slot[0] += int(row["bytes"] or 0)
            else:
                slot[1] += int(row["bytes"] or 0)
        for month, (objects, api) in aggregate.items():
            db.execute("INSERT OR REPLACE INTO traffic_platform(month,platform,object_bytes,api_bytes) VALUES(?,?,?,?)",
                       (month, "windows", objects, api))

    def _secret(self, db: sqlite3.Connection) -> bytes:
        row = db.execute("SELECT v FROM meta WHERE k='hmac_secret'").fetchone()
        return bytes.fromhex(row["v"])

    def client_id(self, raw: str) -> str:
        if not raw:
            return ""
        with self.connect() as db:
            secret = self._secret(db)
        return hmac.new(secret, raw.encode("utf-8"), hashlib.sha256).hexdigest()[:20]

    def event(
        self, kind: str, ip: str = "", *, client: str = "", version: str = "",
        path: str = "", status: int = 200, byte_count: int = 0,
        detail: dict[str, Any] | None = None, now: float | None = None, platform: str = "",
    ) -> int:
        ts = time.time() if now is None else now
        clean_detail = sanitize_text(json.dumps(detail or {}, ensure_ascii=False, separators=(",", ":")))
        platform = platform if platform in ("windows", "mac") else "windows"
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            cursor = db.execute(
                "INSERT INTO events(ts,day,kind,ip,client,version,path,status,bytes,detail,platform) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                (ts, day_for(ts), kind, ip[:160], safe_client(client), version[:80], path[:300], status, max(0, byte_count), clean_detail, platform),
            )
            db.execute(
                "INSERT INTO daily(day,kind,requests,bytes,errors) VALUES(?,?,?,?,?) "
                "ON CONFLICT(day,kind) DO UPDATE SET requests=requests+excluded.requests,"
                "bytes=bytes+excluded.bytes,errors=errors+excluded.errors",
                (day_for(ts), kind, 1, max(0, byte_count), int(status >= 400)),
            )
            db.execute("COMMIT")
            return int(cursor.lastrowid)

    def _traffic_add(self, db: sqlite3.Connection, month: str, column: str, amount: int,
                     platform: str = "windows") -> None:
        if column not in ("object_bytes", "api_bytes"):
            raise ValueError("invalid traffic column")
        db.execute(
            f"INSERT INTO traffic(month,{column}) VALUES(?,?) ON CONFLICT(month) DO UPDATE SET {column}={column}+excluded.{column}",
            (month, max(0, amount)),
        )
        platform = platform if platform in ("windows", "mac") else "windows"
        db.execute(
            f"INSERT INTO traffic_platform(month,platform,{column}) VALUES(?,?,?) "
            f"ON CONFLICT(month,platform) DO UPDATE SET {column}={column}+excluded.{column}",
            (month, platform, max(0, amount)),
        )

    def api_response(self, path: str, status: int, byte_count: int, ip: str, version: str = "",
                     client: str = "", platform: str = "windows", reservation: str = "") -> None:
        if not reservation and byte_count:
            reservation = self.reserve_api(byte_count, platform)
        if reservation:
            self.settle_object(reservation, byte_count)
        self.event("api", ip, path=path, status=status, byte_count=byte_count, version=version,
                   client=client, platform=platform)

    def _expire_reservations(self, db: sqlite3.Connection, now: float) -> None:
        rows = db.execute(
            "SELECT id,month,object,expected,platform FROM reservations WHERE state='active' AND created<?",
            (now - self.config.reservation_ttl,),
        ).fetchall()
        for row in rows:
            changed = db.execute(
                "UPDATE reservations SET state='expired',actual=expected WHERE id=? AND state='active'", (row["id"],)
            ).rowcount
            if changed:
                column = "api_bytes" if str(row["object"]).startswith("api:") else "object_bytes"
                self._traffic_add(db, row["month"], column, row["expected"], str(row["platform"] or "windows"))

    def reserve_object(self, object_id: str, amount: int, platform: str = "windows") -> str:
        if object_id.startswith("api:"):
            raise ValueError("reserved accounting namespace")
        return self._reserve_traffic(object_id, amount, platform)

    def reserve_api(self, amount: int, platform: str = "windows") -> str:
        # Reuse the existing reservation schema; real object IDs are hex hashes.
        return self._reserve_traffic("api:response", amount, platform)

    def _reserve_traffic(self, object_id: str, amount: int, platform: str) -> str:
        amount = int(amount)
        if amount < 0:
            raise ValueError("reservation bytes must not be negative")
        now = time.time()
        month = month_for(now)
        reservation = uuid.uuid4().hex
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            self._expire_reservations(db, now)
            used, reserved = self._monthly_usage(db, month)
            if used + reserved + amount > self.config.monthly_total_bytes:
                db.execute("COMMIT")
                raise HTTPFailure(429, "monthly total traffic quota exceeded", "quota_exceeded", seconds_to_next_month(now))
            db.execute(
                "INSERT INTO reservations(id,month,object,expected,state,created,platform) VALUES(?,?,?,?,'active',?,?)",
                (reservation, month, object_id, amount, now, platform if platform in ("windows", "mac") else "windows"),
            )
            db.execute("COMMIT")
        ratio = (used + reserved + amount) / self.config.monthly_total_bytes
        if ratio >= 0.8:
            self.event("quota_warning", path=object_id, detail={"ratio": round(ratio, 5), "reservation": reservation})
        return reservation

    def settle_object(self, reservation: str, actual: int) -> bool:
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT month,object,expected,state,platform FROM reservations WHERE id=?", (reservation,)).fetchone()
            if row is None or row["state"] != "active":
                db.execute("COMMIT")
                return False
            counted = min(max(0, int(actual)), int(row["expected"]))
            db.execute("UPDATE reservations SET state='settled',actual=? WHERE id=?", (counted, reservation))
            column = "api_bytes" if str(row["object"]).startswith("api:") else "object_bytes"
            self._traffic_add(db, row["month"], column, counted, str(row["platform"] or "windows"))
            db.execute("COMMIT")
            return True

    def api_quota_check(self) -> None:
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            self._expire_reservations(db, time.time())
            used, reserved = self._monthly_usage(db, month_for())
            db.execute("COMMIT")
        if used + reserved >= self.config.monthly_total_bytes:
            raise HTTPFailure(429, "monthly total traffic quota exceeded", "quota_exceeded", seconds_to_next_month())

    @staticmethod
    def _monthly_usage(db: sqlite3.Connection, month: str) -> tuple[int, int]:
        row = db.execute("SELECT object_bytes,api_bytes FROM traffic WHERE month=?", (month,)).fetchone()
        used = int(row["object_bytes"]) + int(row["api_bytes"]) if row else 0
        reserved = int(db.execute(
            "SELECT COALESCE(SUM(expected),0) AS n FROM reservations WHERE state='active' AND month=?", (month,)
        ).fetchone()["n"])
        return used, reserved

    def _traffic_quota_fields(self, objects: int, api: int, reserved: int) -> dict[str, Any]:
        total = objects + api
        ratio = (total + reserved) / self.config.monthly_total_bytes
        return {
            "totalBytes": total, "totalLimit": self.config.monthly_total_bytes,
            "totalCommitted": total + reserved, "ratio": ratio, "limitScope": "shared-total",
            # Compatibility aliases denote the same shared ceiling, not two allowances.
            "objectLimit": self.config.monthly_total_bytes, "apiLimit": self.config.monthly_total_bytes,
            "warning80": ratio >= 0.8, "incrementalOnly90": ratio >= 0.9,
        }

    def object_bytes(self) -> int:
        total = 0
        for directory in (self.objects, self.mac_objects):
            if not directory.is_dir():
                continue
            total += sum(path.stat().st_size for path in directory.iterdir()
                         if path.is_file() and OBJECT_RE.fullmatch(path.name.removesuffix(".zip")))
        return total

    def feedback_bytes(self) -> int:
        with self.connect() as db:
            return int(db.execute("SELECT COALESCE(SUM(diag_size),0) AS n FROM feedback").fetchone()["n"])

    def feedback_recent(self, limit: int = 10) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute(
                "SELECT id,ts,version,os,diag_path,diag_size FROM feedback ORDER BY ts DESC LIMIT ?",
                (max(1, min(50, int(limit))),),
            ).fetchall()
        return [{"id": row["id"], "ts": row["ts"], "time": utc_iso(row["ts"]), "version": row["version"],
                 "os": row["os"], "diagStored": bool(row["diag_path"]), "diagBytes": row["diag_size"]}
                for row in rows]

    def feedback_detail(self, ident: str) -> dict[str, Any]:
        if not re.fullmatch(r"FB-[0-9]{8}-[0-9A-F]{10}", ident):
            raise HTTPFailure(400, "invalid feedback id", "bad_request")
        with self.connect() as db:
            row = db.execute("SELECT * FROM feedback WHERE id=?", (ident,)).fetchone()
        if row is None:
            raise HTTPFailure(404, "feedback not found", "not_found")
        platform = row["platform"] or ("mac" if re.search(r"mac|darwin", str(row["os"]), re.I) else "windows")
        return {"id": row["id"], "ts": row["ts"], "time": utc_iso(row["ts"]), "text": row["text"],
                "version": row["version"], "os": row["os"], "client": row["client"], "platform": platform,
                "diagStored": bool(row["diag_path"]), "diagBytes": row["diag_size"]}

    def feedback_attachment(self, ident: str) -> Path:
        detail = self.feedback_detail(ident)
        if not detail["diagStored"]:
            raise HTTPFailure(404, "diagnostic attachment not found", "not_found")
        with self.connect() as db:
            row = db.execute("SELECT diag_path FROM feedback WHERE id=?", (ident,)).fetchone()
        path = (self.feedback_dir / row["diag_path"]).resolve()
        if path.parent != self.feedback_dir.resolve() or not path.is_file():
            raise HTTPFailure(404, "diagnostic attachment not found", "not_found")
        return path

    def storage_bytes(self) -> int:
        return directory_size(self.root)

    def update_disk_bytes(self) -> int:
        return directory_size(self.root / "update")

    @staticmethod
    def normalize_platform(value: Any) -> str:
        text = str(value or "").strip().lower()
        return "mac" if text in ("mac", "macos", "darwin", "osx") else "windows"

    def platform_used(self, platform: str) -> int:
        directory = self.mac_objects if self.normalize_platform(platform) == "mac" else self.objects
        if not directory.is_dir():
            return 0
        return sum(path.stat().st_size for path in directory.iterdir()
                   if path.is_file() and OBJECT_RE.fullmatch(path.name.removesuffix(".zip")))

    def platform_limit(self, platform: str) -> int:
        if self.normalize_platform(platform) == "mac":
            return int(self.config.macos_update_bytes)
        return int(self.config.windows_update_bytes)

    def platform_storage(self) -> dict[str, Any]:
        windows = self.platform_used("windows")
        macos = self.platform_used("mac")
        feedback = self.feedback_bytes()
        return {
            "windows": {"used": windows, "limit": self.platform_limit("windows")},
            "macos": {"used": macos, "limit": self.platform_limit("mac")},
            "feedback": {"used": feedback, "limit": int(self.config.feedback_storage_bytes)},
            "total": {"used": self.storage_bytes(),
                      "limit": self.platform_limit("windows") + self.platform_limit("mac")
                               + int(self.config.feedback_storage_bytes)},
            "objects": windows + macos,
            "updateTree": self.update_disk_bytes(),
        }

    def submit_feedback(self, payload: dict[str, Any], ip: str, platform: str = "windows") -> tuple[int, dict[str, Any]]:
        required = ("text", "version", "os", "diag", "client", "idempotency_key")
        if any(key not in payload for key in required):
            raise HTTPFailure(400, "missing feedback fields")
        if not isinstance(payload["text"], str) or not payload["text"].strip():
            raise HTTPFailure(400, "text must be a non-empty string")
        if len(payload["text"].encode("utf-8")) > MAX_TEXT:
            raise HTTPFailure(413, "text exceeds 16 KiB", "request_too_large", 60)
        for key in ("version", "os", "client", "idempotency_key"):
            if not isinstance(payload[key], str):
                raise HTTPFailure(400, f"{key} must be a string")
        if not IDEMPOTENCY_RE.fullmatch(payload["idempotency_key"]):
            raise HTTPFailure(400, "invalid idempotency_key")
        if not payload["version"].strip() or len(payload["version"]) > 80 or len(payload["os"]) > 120:
            raise HTTPFailure(400, "invalid version or os")
        if not isinstance(payload["diag"], str):
            raise HTTPFailure(400, "diag must be base64")
        try:
            diag = base64.b64decode(payload["diag"], validate=True)
        except (ValueError, base64.binascii.Error):
            raise HTTPFailure(400, "invalid base64 diagnostic") from None
        if len(diag) > MAX_DIAG:
            raise HTTPFailure(413, "diagnostic exceeds 8 MiB", "request_too_large", 60)
        if diag and not diag.startswith((b"{", b"[")):
            raise HTTPFailure(422, "diagnostic must be JSON", "unsupported_diagnostic")
        platform = self.normalize_platform(payload.get("platform") or platform)
        if platform == "windows" and re.search(r"mac|darwin", str(payload["os"]), re.I):
            platform = "mac"
        clean_text = sanitize_text(sanitize_text(payload["text"]))
        clean_diag = sanitize_diag(sanitize_diag(diag))
        client = self.client_id(safe_client(payload["client"]))
        version = payload["version"].strip()
        ident = ""
        temporary: Path | None = None
        final: Path | None = None
        try:
            with self.connect() as db:
                db.execute("BEGIN IMMEDIATE")
                existing = db.execute(
                    "SELECT id,diag_path FROM feedback WHERE idem=?", (payload["idempotency_key"],)
                ).fetchone()
                if existing:
                    db.execute("COMMIT")
                    return 200, {"ok": True, "id": existing["id"], "diagStored": bool(existing["diag_path"])}
                physical = self.storage_bytes()
                non_update = max(0, physical - self.update_disk_bytes())
                store_diag = bool(clean_diag) and non_update + len(clean_diag) <= self.config.feedback_storage_bytes
                store_diag = store_diag and physical + len(clean_diag) <= self.config.storage_bytes
                ident = "FB-" + dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d") + "-" + secrets.token_hex(5).upper()
                if store_diag:
                    final = self.feedback_dir / (ident + ".json")
                    temporary = final.with_suffix(final.suffix + ".tmp")
                    with temporary.open("xb") as handle:
                        handle.write(clean_diag)
                        handle.flush()
                        os.fsync(handle.fileno())
                    os.replace(temporary, final)
                db.execute(
                    "INSERT INTO feedback(id,idem,ts,text,version,os,client,diag_path,diag_size,platform) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?)",
                    (ident, payload["idempotency_key"], time.time(), clean_text, version, payload["os"], client,
                     final.name if store_diag and final else None, len(clean_diag) if store_diag else 0, platform),
                )
                db.execute("COMMIT")
        except sqlite3.IntegrityError:
            if final:
                final.unlink(missing_ok=True)
            with self.connect() as db:
                existing = db.execute("SELECT id,diag_path FROM feedback WHERE idem=?", (payload["idempotency_key"],)).fetchone()
            if existing:
                return 200, {"ok": True, "id": existing["id"], "diagStored": bool(existing["diag_path"])}
            raise
        except Exception:
            if final:
                final.unlink(missing_ok=True)
            raise
        finally:
            if temporary:
                temporary.unlink(missing_ok=True)
        result = {"ok": True, "id": ident, "diagStored": bool(final)}
        if not final and clean_diag:
            result["diagReason"] = "feedback_storage_quota"
        self.event("feedback", ip, client=client, version=version,
                   path="/api/v1/feedback/%s" % ("macos" if platform == "mac" else "windows"),
                   detail={"id": ident, "diagStored": bool(final)}, platform=platform)
        return 201, result

    def report_update(self, payload: dict[str, Any], ip: str, platform: str = "windows") -> None:
        platform = "mac" if platform in ("mac", "macos") else "windows"
        if not isinstance(payload.get("from"), str) or not isinstance(payload.get("to"), str):
            raise HTTPFailure(400, "from and to versions are required")
        if not isinstance(payload.get("ok"), bool) or not isinstance(payload.get("ms"), (int, float)):
            raise HTTPFailure(400, "ok must be boolean and ms numeric")
        if not 0 <= payload["ms"] <= 86_400_000 or len(payload["from"]) > 80 or len(payload["to"]) > 80:
            raise HTTPFailure(400, "invalid update report")
        client = self.client_id(safe_client(payload.get("client")))
        with self.connect() as db:
            db.execute(
                "INSERT INTO reports(ts,from_version,to_version,ok,ms,client) VALUES(?,?,?,?,?,?)",
                (time.time(), payload["from"], payload["to"], int(payload["ok"]), int(payload["ms"]), client),
            )
        self.event("update_report", ip, client=client, version=payload["to"],
                   path="/api/v1/update/%s/report" % ("macos" if platform == "mac" else "windows"),
                   detail={"from": payload["from"], "ok": payload["ok"], "ms": int(payload["ms"])},
                   platform=platform)

    # ---------------- 管理员账号（用户名 + 密码，PBKDF2-SHA256） ----------------
    @staticmethod
    def _password_digest(password: str, salt: bytes, iterations: int) -> str:
        return hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, iterations).hex()

    def set_user(self, username: str, password: str, role: str = "admin", iterations: int = 0,
                 keep_token: str = "") -> None:
        username = (username or "").strip()
        if not USERNAME_RE.fullmatch(username):
            raise ValueError("username must be 1..32 characters of letters, digits, dot, dash or underscore")
        if not isinstance(password, str) or not 8 <= len(password) <= 200:
            raise ValueError("password must be 8..200 characters")
        iterations = max(100_000, int(iterations or self.config.pbkdf2_iterations))
        salt = secrets.token_bytes(16)
        digest = self._password_digest(password, salt, iterations)
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            db.execute(
                "INSERT INTO users(username,salt,hash,iterations,role,created) VALUES(?,?,?,?,?,?) "
                "ON CONFLICT(username) DO UPDATE SET salt=excluded.salt,hash=excluded.hash,"
                "iterations=excluded.iterations,role=excluded.role",
                (username, salt.hex(), digest, iterations, role, time.time()),
            )
            if keep_token:
                db.execute("DELETE FROM sessions WHERE username=? AND token_hash<>?",
                           (username, hashlib.sha256(keep_token.encode()).hexdigest()))
            else:
                db.execute("DELETE FROM sessions WHERE username=?", (username,))
            db.execute("COMMIT")

    def user_count(self) -> int:
        with self.connect() as db:
            return int(db.execute("SELECT COUNT(*) AS n FROM users").fetchone()["n"])

    def users(self) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute("SELECT username,role,created FROM users ORDER BY created, username").fetchall()
        return [{"username": row["username"], "role": row["role"], "created": utc_iso(row["created"])}
                for row in rows]

    def user_role(self, username: str) -> str:
        with self.connect() as db:
            row = db.execute("SELECT role FROM users WHERE username=?", (username,)).fetchone()
        return str(row["role"]) if row else ""

    def verify_user(self, username: str, password: str) -> bool:
        with self.connect() as db:
            row = db.execute("SELECT salt,hash,iterations FROM users WHERE username=?", (username,)).fetchone()
        if row is None:
            self._password_digest(password, b"0" * 16, min(600_000, self.config.pbkdf2_iterations))
            return False
        try:
            salt = bytes.fromhex(row["salt"])
        except ValueError:
            return False
        actual = self._password_digest(password, salt, max(100_000, int(row["iterations"])))
        return hmac.compare_digest(actual, str(row["hash"]))

    def change_password(self, username: str, current: str, new: str, keep_token: str = "") -> bool:
        if not self.verify_user(username, current):
            return False
        role = self.user_role(username) or "admin"
        self.set_user(username, new, role=role, keep_token=keep_token)
        return True

    def login_csrf(self) -> str:
        timestamp = str(int(time.time()))
        with self.connect() as db:
            secret = self._secret(db)
        signature = hmac.new(secret, ("login:" + timestamp).encode(), hashlib.sha256).hexdigest()
        return timestamp + "." + signature

    def check_login_csrf(self, token: str) -> bool:
        try:
            timestamp, signature = token.split(".", 1)
            if abs(time.time() - int(timestamp)) > 900:
                return False
            with self.connect() as db:
                secret = self._secret(db)
            expected = hmac.new(secret, ("login:" + timestamp).encode(), hashlib.sha256).hexdigest()
            return hmac.compare_digest(signature, expected)
        except (ValueError, TypeError):
            return False

    def create_session(self, username: str) -> tuple[str, str]:
        token, csrf = secrets.token_urlsafe(32), secrets.token_urlsafe(32)
        with self.connect() as db:
            db.execute("DELETE FROM sessions WHERE expires<?", (time.time(),))
            db.execute("INSERT INTO sessions(token_hash,csrf,expires,username) VALUES(?,?,?,?)",
                       (hashlib.sha256(token.encode()).hexdigest(), csrf,
                        time.time() + self.config.session_ttl, username))
        return token, csrf

    def session_info(self, token: str) -> tuple[str, str] | None:
        if not token:
            return None
        with self.connect() as db:
            row = db.execute("SELECT csrf,expires,username FROM sessions WHERE token_hash=?",
                             (hashlib.sha256(token.encode()).hexdigest(),)).fetchone()
        if row is None or row["expires"] < time.time():
            return None
        return str(row["username"]), str(row["csrf"])

    def session(self, token: str) -> str | None:
        info = self.session_info(token)
        return info[1] if info else None

    def logout(self, token: str) -> None:
        with self.connect() as db:
            db.execute("DELETE FROM sessions WHERE token_hash=?", (hashlib.sha256(token.encode()).hexdigest(),))

    # ---------------- 统计口径 ----------------
    def range_bounds(self, name: str) -> tuple[float, float]:
        """日 / 周 / 月 均按 UTC+8 自然日、自然周（周一起）、自然月。"""
        now = time.time()
        local = dt.datetime.fromtimestamp(now, CN_TZ)
        name = name if name in ("day", "week", "month", "all") else "day"
        if name == "all":
            return 0.0, now
        if name == "day":
            start = local.replace(hour=0, minute=0, second=0, microsecond=0)
        elif name == "week":
            start = (local - dt.timedelta(days=local.weekday())).replace(hour=0, minute=0, second=0, microsecond=0)
        else:
            start = local.replace(day=1, hour=0, minute=0, second=0, microsecond=0)
        return start.timestamp(), now

    # ---------------- macOS 增量发布（第三方发行者令牌） ----------------
    @staticmethod
    def _safe_rel(path: Any) -> bool:
        if not isinstance(path, str) or not path or len(path) > 400:
            return False
        if path.startswith("/") or "\\" in path or "\x00" in path or path.endswith("/"):
            return False
        parts = path.split("/")
        return all(part not in ("", ".", "..") for part in parts)

    @staticmethod
    def _parse_iso(value: Any) -> float | None:
        if not isinstance(value, str) or not value.endswith("Z") or len(value) > 40:
            return None
        for pattern in ("%Y-%m-%dT%H:%M:%S.%fZ", "%Y-%m-%dT%H:%M:%SZ"):
            try:
                return dt.datetime.strptime(value, pattern).replace(tzinfo=dt.timezone.utc).timestamp()
            except ValueError:
                continue
        return None

    def macos_release_history(self) -> list[dict[str, Any]]:
        found: list[dict[str, Any]] = []
        if not self.mac_releases.is_dir():
            return found
        for path in sorted(self.mac_releases.glob("*.json")):
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, UnicodeError, json.JSONDecodeError):
                continue
            if not isinstance(data, dict):
                continue
            found.append({"version": str(data.get("version") or path.stem),
                          "publishedAt": str(data.get("published_at") or ""),
                          "installMode": str(data.get("install_mode") or ""),
                          "files": len(data.get("files") or [])})
        return found

    def macos_latest_payload(self) -> dict[str, Any] | None:
        try:
            envelope = json.loads(self.mac_latest.read_bytes().decode("utf-8"))
            payload = base64.urlsafe_b64decode(str(envelope["payload"]) + "=" * (-len(str(envelope["payload"])) % 4))
            value = json.loads(payload.decode("utf-8"))
            return value if isinstance(value, dict) else None
        except Exception:
            return None

    def store_macos_object(self, sha: str, data: bytes) -> dict[str, Any]:
        if not OBJECT_RE.fullmatch(sha):
            raise HTTPFailure(400, "object name must be 64 lowercase hex")
        if not data:
            raise HTTPFailure(400, "empty object")
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                names = archive.namelist()
                if names != ["payload.bin"]:
                    raise HTTPFailure(400, "object zip must contain exactly payload.bin")
                payload = archive.read("payload.bin")
        except zipfile.BadZipFile:
            raise HTTPFailure(400, "object is not a valid zip") from None
        if hashlib.sha256(payload).hexdigest() != sha:
            raise HTTPFailure(422, "payload sha256 does not match object name")
        target = self.mac_objects / (sha + ".zip")
        if target.is_file() and target.stat().st_size == len(data):
            return {"ok": True, "object": sha + ".zip", "size": len(data), "already": True}
        mac_used = self.platform_used("mac")
        if mac_used + len(data) > self.platform_limit("mac"):
            raise HTTPFailure(429, "macOS update storage quota exceeded (5 GiB)", "quota_exceeded")
        if directory_size(self.root) + len(data) > self.platform_storage()["total"]["limit"]:
            raise HTTPFailure(429, "storage quota exceeded", "quota_exceeded")
        atomic_write(target, data)
        return {"ok": True, "object": sha + ".zip", "size": len(data), "already": False}

    def publish_macos_manifest(self, raw: bytes, ip: str = "") -> dict[str, Any]:
        if not raw or len(raw) > 8 * 1024 * 1024:
            raise HTTPFailure(413, "manifest body must be 1 byte .. 8 MiB", "request_too_large", 60)
        try:
            envelope = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise HTTPFailure(400, "manifest is not valid UTF-8 JSON", "invalid_json") from None
        if not isinstance(envelope, dict) or set(envelope) != {"alg", "kid", "payload", "signature"}:
            raise HTTPFailure(400, "envelope must contain exactly alg/kid/payload/signature")
        if envelope.get("alg") != "PS256":
            raise HTTPFailure(400, "alg must be PS256")
        kid = envelope.get("kid")
        if not isinstance(kid, str) or not 4 <= len(kid) <= 64:
            raise HTTPFailure(400, "invalid kid")
        try:
            signature = base64.urlsafe_b64decode(str(envelope["signature"]) + "=" * (-len(str(envelope["signature"])) % 4))
            payload = json.loads(base64.urlsafe_b64decode(str(envelope["payload"]) + "=" * (-len(str(envelope["payload"])) % 4)).decode("utf-8"))
        except (ValueError, TypeError, UnicodeDecodeError, json.JSONDecodeError, base64.binascii.Error):
            raise HTTPFailure(400, "payload/signature are not base64url JSON") from None
        if not isinstance(payload, dict):
            raise HTTPFailure(400, "payload must be a JSON object")
        if payload.get("platform") != "macos":
            raise HTTPFailure(400, "payload platform must be macos")
        if payload.get("schema") != 2 or payload.get("metadata_version") != 1 or payload.get("channel") != "stable":
            raise HTTPFailure(400, "payload schema/metadata_version/channel mismatch")
        version = payload.get("version")
        if not isinstance(version, str) or not MACOS_VERSION_RE.fullmatch(version):
            raise HTTPFailure(400, "invalid version")
        published = self._parse_iso(payload.get("published_at"))
        expires = self._parse_iso(payload.get("expires_at"))
        now = time.time()
        if published is None or expires is None or expires <= now or published > now + 86400:
            raise HTTPFailure(400, "published_at/expires_at invalid or already expired")
        mode = payload.get("install_mode")
        if mode not in ("incremental", "manual"):
            raise HTTPFailure(400, "install_mode must be incremental or manual")
        files = payload.get("files")
        if not isinstance(files, list):
            raise HTTPFailure(400, "files must be a list")
        if mode == "incremental" and not files:
            raise HTTPFailure(400, "incremental release must describe files")
        total_size = 0
        for item in files:
            if not isinstance(item, dict):
                raise HTTPFailure(400, "file entry must be an object")
            path = item.get("path")
            sha = item.get("sha256")
            size = item.get("size")
            if not self._safe_rel(path):
                raise HTTPFailure(400, "unsafe file path")
            if not isinstance(sha, str) or not OBJECT_RE.fullmatch(sha):
                raise HTTPFailure(400, "invalid file sha256")
            if not isinstance(size, int) or size < 0:
                raise HTTPFailure(400, "invalid file size")
            if item.get("object") != sha + ".zip":
                raise HTTPFailure(400, "object name must equal sha256.zip")
            if not isinstance(item.get("object_sha256"), str) or not OBJECT_RE.fullmatch(item["object_sha256"]):
                raise HTTPFailure(400, "invalid object_sha256")
            if not isinstance(item.get("object_size"), int) or item["object_size"] <= 0:
                raise HTTPFailure(400, "invalid object_size")
            mode_bits = item.get("mode")
            if mode_bits is not None and not (isinstance(mode_bits, str) and re.fullmatch(r"[0-7]{3,4}", mode_bits)):
                raise HTTPFailure(400, "invalid mode")
            stored = self.mac_objects / (sha + ".zip")
            if not stored.is_file():
                raise HTTPFailure(409, "object not uploaded yet: " + sha)
            if stored.stat().st_size != item["object_size"]:
                raise HTTPFailure(409, "object size mismatch: " + sha)
            total_size += size
        deletes = payload.get("delete") or []
        if not isinstance(deletes, list) or any(not self._safe_rel(item) for item in deletes):
            raise HTTPFailure(400, "invalid delete list")
        manual = payload.get("manual")
        if not isinstance(manual, dict):
            raise HTTPFailure(400, "manual rescue links are required")
        for key in ("cn_url", "global_url"):
            value = manual.get(key)
            if not isinstance(value, str) or not value.startswith("https://") or len(value) > 2000:
                raise HTTPFailure(400, "manual.%s must be an https URL" % key)
        if not isinstance(manual.get("sha256"), str) or not OBJECT_RE.fullmatch(manual["sha256"]):
            raise HTTPFailure(400, "manual.sha256 must be 64 hex")
        if not isinstance(manual.get("size"), int) or manual["size"] <= 0:
            raise HTTPFailure(400, "manual.size must be positive")
        atomic_write(self.mac_latest, raw)
        record = json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
        atomic_write(self.mac_releases / (version + ".json"), record)
        self.prune_macos()
        self.event("publish_macos", ip, path="/admin/api/publish/macos/latest",
                   detail={"version": version, "files": len(files), "kid": kid}, platform="mac")
        return {"ok": True, "version": version, "files": len(files), "bytes": total_size,
                "objects": len({item["object"] for item in files}), "kid": kid}

    def prune_macos(self, keep: int = 3) -> None:
        entries: list[tuple[str, Path, dict[str, Any]]] = []
        for path in self.mac_releases.glob("*.json"):
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, UnicodeError, json.JSONDecodeError):
                continue
            if isinstance(data, dict):
                entries.append((str(data.get("published_at") or ""), path, data))
        entries.sort(key=lambda item: (item[0], item[1].stem))
        kept = entries[-keep:] if len(entries) > keep else entries
        kept_versions = {item[1].stem for item in kept}
        for _, path, _data in entries:
            if path.stem not in kept_versions:
                path.unlink(missing_ok=True)
        referenced = set()
        for _, _path, data in kept:
            for item in data.get("files") or []:
                if isinstance(item, dict) and isinstance(item.get("sha256"), str):
                    referenced.add(item["sha256"])
        cutoff = time.time() - 86400
        for path in self.mac_objects.glob("*.zip"):
            if path.stem in referenced:
                continue
            try:
                if path.stat().st_mtime > cutoff:
                    continue
            except OSError:
                continue
            path.unlink(missing_ok=True)

    def release_history(self) -> list[dict[str, Any]]:
        releases = self.root / "update" / "releases"
        found: list[dict[str, Any]] = []
        if not releases.is_dir():
            return found
        for path in sorted(releases.glob("*.json")):
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, UnicodeError, json.JSONDecodeError):
                continue
            if not isinstance(data, dict):
                continue
            found.append({
                "version": str(data.get("version") or path.stem),
                "publishedAt": str(data.get("published_at") or ""),
                "installMode": str(data.get("install_mode") or "incremental"),
                "files": len(data.get("files") or []),
            })
        return found

    @staticmethod
    def _version_key(value: str) -> tuple:
        parts = []
        for token in re.split(r"[.\-+]", str(value or "")):
            parts.append((0, int(token)) if token.isdigit() else (1, token))
        return tuple(parts)

    def incremental_since(self) -> str:
        versions = [item["version"] for item in self.release_history() if item.get("installMode") == "incremental"]
        if versions:
            return min(versions, key=self._version_key)
        try:
            data = json.loads(self.latest.read_text(encoding="utf-8"))
            payload = json.loads(base64.urlsafe_b64decode(data["payload"] + "=" * (-len(data["payload"]) % 4)))
            return str(payload.get("version") or "3.2.2")
        except Exception:
            return "3.2.2"

    def admin_version(self) -> str:
        versions = [item["version"] for item in self.release_history()]
        return max(versions, key=self._version_key) if versions else "3.2.2"

    def overview(self) -> dict[str, Any]:
        self.cleanup()
        month = month_for()
        day_start, _ = self.range_bounds("day")
        week_start, _ = self.range_bounds("week")
        month_start, now = self.range_bounds("month")
        with self.connect() as db:
            def platform_row(row: sqlite3.Row | None) -> dict[str, int]:
                return {"objectBytes": int(row["object_bytes"]) if row else 0,
                        "apiBytes": int(row["api_bytes"]) if row else 0}
            per_platform = {row["platform"]: row for row in
                            db.execute("SELECT * FROM traffic_platform WHERE month=?", (month,)).fetchall()}
            total = db.execute("SELECT * FROM traffic WHERE month=?", (month,)).fetchone()
            reserved = db.execute("SELECT COALESCE(SUM(expected),0) AS n,COUNT(*) AS c,"
                                  "COALESCE(SUM(CASE WHEN object LIKE 'api:%' THEN expected ELSE 0 END),0) AS api "
                                  "FROM reservations "
                                  "WHERE state='active' AND month=?", (month,)).fetchone()
            errors = int(db.execute("SELECT COUNT(*) AS n FROM events WHERE status>=400 AND ts>=?",
                                    (now - 86400,)).fetchone()["n"])
            blocked = int(db.execute("SELECT COUNT(*) AS n FROM events WHERE kind='blocked_download' AND ts>=?",
                                     (now - 86400,)).fetchone()["n"])
            external = {row["target"] or "unknown": int(row["n"]) for row in db.execute(
                "SELECT json_extract(detail,'$.target') AS target,COUNT(*) AS n FROM events "
                "WHERE kind='external_click' AND ts>=? GROUP BY target", (month_start,)).fetchall()}
            feedback = {name: int(db.execute("SELECT COUNT(*) AS n FROM feedback WHERE ts>=?",
                                             (value,)).fetchone()["n"])
                        for name, value in (("day", day_start), ("week", week_start), ("month", month_start))}
            feedback["total"] = int(db.execute("SELECT COUNT(*) AS n FROM feedback").fetchone()["n"])
            versions = [{"version": row["version"], "count": int(row["n"])} for row in db.execute(
                "SELECT version,COUNT(*) AS n FROM events WHERE version<>'' GROUP BY version ORDER BY n DESC LIMIT 30")]
            recent = db.execute("SELECT * FROM events ORDER BY id DESC LIMIT 30").fetchall()
            today = dt.datetime.fromtimestamp(now, CN_TZ).date()
            days = [(today - dt.timedelta(days=offset)).isoformat() for offset in range(13, -1, -1)]
            trend_start = dt.datetime.combine(today - dt.timedelta(days=13), dt.time.min, CN_TZ).timestamp()
            daily = {row["day"]: row for row in db.execute(
                "SELECT day,SUM(CASE WHEN kind='object_download' THEN bytes ELSE 0 END) AS object_bytes,"
                "SUM(CASE WHEN kind='feedback' THEN 1 ELSE 0 END) AS feedback FROM daily "
                "WHERE day>=? GROUP BY day", (days[0],)).fetchall()}
            feedback_days = {row["day"]: int(row["n"]) for row in db.execute(
                "SELECT substr(datetime(ts,'unixepoch','+8 hours'),1,10) AS day,COUNT(*) AS n "
                "FROM feedback WHERE ts>=? GROUP BY day", (trend_start,)).fetchall()}
        windows = platform_row(per_platform.get("windows"))
        mac = platform_row(per_platform.get("mac"))
        object_month = int(total["object_bytes"]) if total else windows["objectBytes"] + mac["objectBytes"]
        api_month = int(total["api_bytes"]) if total else windows["apiBytes"] + mac["apiBytes"]
        # 分平台表是后加的：历史差额（全部产生于 macOS 接入之前）记到 Windows，保证 总量 == 各平台之和
        for key, month_value in (("objectBytes", object_month), ("apiBytes", api_month)):
            gap = month_value - (windows[key] + mac[key])
            if gap > 0:
                windows[key] += gap
            elif gap < 0:
                if key == "objectBytes":
                    object_month = windows[key] + mac[key]
                else:
                    api_month = windows[key] + mac[key]
        blacklist = self.blacklist_summary()
        since = self.incremental_since()
        versions = [dict(item, incremental=(self._version_key(item["version"]) >= self._version_key(since))
                         if since else True) for item in versions]
        return {
            "ok": True, "time": utc_iso(now), "month": month, "timezone": "Asia/Shanghai",
            "traffic": {
                "windows": windows, "mac": mac, "objectBytes": object_month, "apiBytes": api_month,
                "reserved": int(reserved["n"]), "activeReservations": int(reserved["c"]),
                "objectReserved": int(reserved["n"]) - int(reserved["api"]), "apiReserved": int(reserved["api"]),
                **self._traffic_quota_fields(object_month, api_month, int(reserved["n"])),
                "retryAfter": seconds_to_next_month(now),
            },
            "storage": self.platform_storage(),
            "versions": versions, "incrementalSince": since, "releases": self.release_history(),
            "macosReleases": self.macos_release_history(),
            "macosLatest": (self.macos_latest_payload() or {}).get("version", ""),
            "feedback": feedback, "errors24h": errors, "blocked24h": blocked,
            "external": {"baidu": external.get("baidu", 0),
                         "githubWindows": external.get("github-windows", 0),
                         "githubMacos": external.get("github-macos", 0),
                         "github": external.get("github-windows", 0) + external.get("github-macos", 0),
                         "total": sum(external.values())},
            "blacklist": blacklist,
            "recentEvents": [self.public_event(row) for row in recent],
            "trend": {"days": days,
                      "objectBytes": [int((daily.get(day)["object_bytes"] or 0) if day in daily else 0) for day in days],
                      "feedback": [int(feedback_days.get(day, 0)) for day in days]},
        }

    def download_rows(self, range_name: str = "day", platform: str = "all", dimension: str = "ip",
                      query: str = "", limit: int = 20, offset: int = 0, sort: str = "requests") -> dict[str, Any]:
        start, now = self.range_bounds(range_name)
        dimension = "client" if dimension == "client" else "ip"
        clauses = ["kind='object_download'", "ts>=?", "ts<=?"]
        # A client dimension must use the same population for rows, pagination
        # and byte/request totals. Older clients without an ID remain in IP stats.
        if dimension == "client":
            clauses.append("client<>''")
        args: list[Any] = [start, now]
        if platform in ("windows", "mac"):
            clauses.append("platform=?")
            args.append(platform)
        needle = (query or "").strip()
        if needle:
            clauses.append("%s LIKE ?" % dimension)
            args.append(needle.replace("*", "%").rstrip("%") + "%")
        clause = " AND ".join(clauses)
        order = "bytes" if sort == "bytes" else "requests"
        entries = self.blocked_entries()
        rows: list[dict[str, Any]] = []
        if dimension == "ip":
            # 按打码后的 /24 段聚合，避免同一网段出现多行相同显示值；黑名单也按这个粒度生效
            with self.connect() as db:
                raw = db.execute(
                    "SELECT ip,COUNT(*) AS requests,COALESCE(SUM(bytes),0) AS bytes,MAX(ts) AS last "
                    "FROM events WHERE " + clause + " GROUP BY ip", args).fetchall()
            merged: dict[str, dict[str, Any]] = {}
            for row in raw:
                ip = str(row["ip"] or "")
                key = mask_ip(ip)
                slot = merged.setdefault(key, {"value": key, "requests": 0, "bytes": 0, "last": 0.0,
                                               "country": "", "province": "", "city": "", "reserved": False,
                                               "blocked": False})
                slot["requests"] += int(row["requests"])
                slot["bytes"] += int(row["bytes"])
                slot["last"] = max(slot["last"], float(row["last"] or 0))
                if not slot["country"]:
                    info = self.geo.lookup(ip)
                    slot["country"] = info.get("country", "")
                    slot["province"] = info.get("province", "")
                    slot["city"] = info.get("city", "")
                    slot["reserved"] = bool(info.get("reserved"))
                slot["blocked"] = slot["blocked"] or any(
                    self._match_entry(kind, value, ip, "") for kind, value in entries)
            rows = sorted(merged.values(), key=lambda item: (item["bytes"] if order == "bytes" else item["requests"],
                                                             item["last"]), reverse=True)
            total = len(rows)
            totals = {"requests": sum(item["requests"] for item in rows),
                      "bytes": sum(item["bytes"] for item in rows)}
            page_rows = rows[max(0, int(offset)):max(0, int(offset)) + max(1, min(200, int(limit)))]
        else:
            with self.connect() as db:
                total = int(db.execute("SELECT COUNT(DISTINCT client) AS n FROM events WHERE " + clause,
                                       args).fetchone()["n"])
                raw = db.execute(
                    "SELECT client AS key,COUNT(*) AS requests,COALESCE(SUM(bytes),0) AS bytes,MAX(ts) AS last "
                    "FROM events WHERE " + clause + " AND client<>'' GROUP BY client ORDER BY " + order +
                    " DESC,last DESC LIMIT ? OFFSET ?",
                    args + [max(1, min(200, int(limit))), max(0, int(offset))]).fetchall()
                totals = db.execute("SELECT COUNT(*) AS requests,COALESCE(SUM(bytes),0) AS bytes FROM events "
                                    "WHERE " + clause, args).fetchone()
                totals = {"requests": int(totals["requests"]), "bytes": int(totals["bytes"])}
            page_rows = []
            for row in raw:
                key = str(row["key"] or "")
                page_rows.append({"value": key, "requests": int(row["requests"]), "bytes": int(row["bytes"]),
                                  "last": float(row["last"] or 0), "country": "", "province": "", "city": "",
                                  "reserved": False,
                                  "blocked": any(self._match_entry(kind, value, "", key)
                                                 for kind, value in entries)})
        for item in page_rows:
            item["last"] = utc_iso(item["last"])
        pages = max(1, (total + max(1, int(limit)) - 1) // max(1, int(limit)))
        return {"ok": True, "range": range_name, "from": utc_iso(start), "to": utc_iso(now),
                "platform": platform, "dimension": dimension, "total": total, "pages": pages,
                "rows": page_rows, "totals": totals}

    def region_rows(self, range_name: str = "day", platform: str = "all", limit: int = 20) -> dict[str, Any]:
        start, now = self.range_bounds(range_name)
        clauses = ["kind='object_download'", "ts>=?", "ts<=?"]
        args: list[Any] = [start, now]
        if platform in ("windows", "mac"):
            clauses.append("platform=?")
            args.append(platform)
        with self.connect() as db:
            rows = db.execute("SELECT ip,COUNT(*) AS requests,COALESCE(SUM(bytes),0) AS bytes FROM events "
                              "WHERE %s GROUP BY ip" % " AND ".join(clauses), args).fetchall()
        aggregate: dict[tuple[str, str], dict[str, Any]] = {}
        for row in rows:
            info = self.geo.lookup(str(row["ip"] or ""))
            key = (info.get("country", "") or "未知", info.get("province", "") or "")
            slot = aggregate.setdefault(key, {"country": key[0], "province": key[1], "requests": 0,
                                              "bytes": 0, "addresses": 0})
            slot["requests"] += int(row["requests"])
            slot["bytes"] += int(row["bytes"])
            slot["addresses"] += 1
        ordered = sorted(aggregate.values(), key=lambda item: (item["bytes"], item["requests"]), reverse=True)
        return {"ok": True, "range": range_name, "rows": ordered[:max(1, min(100, int(limit)))],
                "total": len(ordered)}

    def feedback_page(self, range_name: str = "all", limit: int = 100, offset: int = 0,
                      platform: str = "all") -> dict[str, Any]:
        start, now = self.range_bounds(range_name)
        platform = platform if platform in ("windows", "mac") else "all"
        clauses: list[str] = []
        args: list[Any] = []
        if range_name != "all":
            clauses.append("ts>=? AND ts<=?")
            args += [start, now]
        if platform != "all":
            clauses.append("platform=?")
            args.append(platform)
        where = " AND ".join(clauses) if clauses else "1=1"
        with self.connect() as db:
            total = int(db.execute("SELECT COUNT(*) AS n FROM feedback WHERE %s" % where, args).fetchone()["n"])
            rows = db.execute("SELECT * FROM feedback WHERE %s ORDER BY ts DESC LIMIT ? OFFSET ?" % where,
                              args + [max(1, min(200, int(limit))), max(0, int(offset))]).fetchall()
            counts = {}
            for name in ("day", "week", "month"):
                query = "SELECT COUNT(*) AS n FROM feedback WHERE ts>=?"
                query_args: list[Any] = [self.range_bounds(name)[0]]
                if platform != "all":
                    query += " AND platform=?"
                    query_args.append(platform)
                counts[name] = int(db.execute(query, query_args).fetchone()["n"])
            counts["total"] = total
            by_platform = {row["platform"] or "windows": int(row["n"]) for row in
                           db.execute("SELECT platform,COUNT(*) AS n FROM feedback GROUP BY platform").fetchall()}
        def title(text: str) -> str:
            line = (text or "").strip().splitlines()
            return line[0][:80] if line else ""
        return {"ok": True, "counts": counts, "total": total, "platform": platform,
                "byPlatform": {"windows": by_platform.get("windows", 0), "mac": by_platform.get("mac", 0),
                               "all": sum(by_platform.values())},
                "rows": [
            {"id": row["id"], "time": utc_iso(row["ts"]), "version": row["version"], "os": row["os"],
             "platform": row["platform"] or ("mac" if re.search(r"mac|darwin", str(row["os"]), re.I) else "windows"),
             "diagStored": bool(row["diag_path"]), "diagBytes": int(row["diag_size"]),
             "title": title(row["text"])} for row in rows]}

    def error_rows(self, hours: int = 24, limit: int = 50) -> dict[str, Any]:
        since = time.time() - max(1, min(24 * 30, int(hours))) * 3600
        with self.connect() as db:
            total = int(db.execute("SELECT COUNT(*) AS n FROM events WHERE status>=400 AND ts>=?",
                                   (since,)).fetchone()["n"])
            rows = db.execute("SELECT * FROM events WHERE status>=400 AND ts>=? ORDER BY id DESC LIMIT ?",
                              (since, max(1, min(200, int(limit))))).fetchall()
        return {"ok": True, "total": total, "rows": [
            {"id": row["id"], "time": utc_iso(row["ts"]), "kind": row["kind"], "status": int(row["status"]),
             "ip": mask_ip(row["ip"]), "path": row["path"], "version": row["version"],
             "client": row["client"]} for row in rows]}

    @staticmethod
    def external_target(value: Any) -> str:
        text = str(value or "").strip().lower()
        if text in ("baidu", "pan", "netdisk"):
            return "baidu"
        if text in ("github", "github-windows", "github-win", "windows-github", "github_win"):
            return "github-windows"
        if text in ("github-macos", "github-mac", "macos-github", "github_mac"):
            return "github-macos"
        return text

    def external_counts(self, range_name: str = "day") -> dict[str, Any]:
        start, now = self.range_bounds(range_name)
        with self.connect() as db:
            rows = db.execute("SELECT json_extract(detail,'$.target') AS target,COUNT(*) AS n FROM events "
                              "WHERE kind='external_click' AND ts>=? AND ts<=? GROUP BY target", (start, now)).fetchall()
        counts: dict[str, int] = {}
        for row in rows:
            key = self.external_target(row["target"])
            counts[key] = counts.get(key, 0) + int(row["n"])
        windows = counts.get("github-windows", 0)
        macos = counts.get("github-macos", 0)
        return {"ok": True, "baidu": counts.get("baidu", 0),
                "githubWindows": windows, "githubMacos": macos,
                "github": windows + macos, "total": sum(counts.values())}

    # ---------------- 黑名单 ----------------
    def blocked_entries(self) -> list[tuple[str, str]]:
        with self.connect() as db:
            rows = db.execute("SELECT kind,value FROM blacklist WHERE active=1").fetchall()
        return [(str(row["kind"]), str(row["value"])) for row in rows]

    @staticmethod
    def _match_entry(kind: str, value: str, ip: str, client: str) -> bool:
        if kind == "client":
            return bool(client) and (client == value or client.startswith(value))
        if not ip:
            return False
        if value.endswith("*"):
            return ip.startswith(value[:-1])
        if "/" in value:
            try:
                return ipaddress.ip_address(ip) in ipaddress.ip_network(value, strict=False)
            except ValueError:
                return False
        return ip == value

    def is_blocked(self, ip: str, client: str = "") -> bool:
        return any(self._match_entry(kind, value, ip, client) for kind, value in self.blocked_entries())

    def _normalize_block_value(self, kind: str, value: str) -> str:
        value = (value or "").strip()
        if not value or len(value) > 64 or any(ch.isspace() for ch in value):
            raise ValueError("黑名单值不能为空、不能超过 64 字符、不能含空白")
        if kind == "client":
            if not re.fullmatch(r"[0-9a-fA-F]{8,40}", value):
                raise ValueError("设备 ID 必须是 8~40 位十六进制（取后台设备列的前缀即可）")
            return value.lower()
        if kind != "ip":
            raise ValueError("kind must be ip or client")
        if value.endswith("*"):
            prefix = value[:-1]
            parts = [part for part in prefix.split(".") if part]
            if not parts or len(parts) > 3 or any(not part.isdigit() or not 0 <= int(part) <= 255 for part in parts):
                raise ValueError("IP 通配写法示例：183.7.144.*")
            return ".".join(parts) + ".*"
        if "/" in value:
            try:
                ipaddress.ip_network(value, strict=False)
            except ValueError as error:
                raise ValueError("无效的 IP 网段：%s" % error) from None
            return value
        try:
            return str(ipaddress.ip_address(value))
        except ValueError:
            raise ValueError("无效的 IPv4 地址（也支持 183.7.144.* 或 183.7.144.0/24）") from None

    def blacklist_rows(self) -> dict[str, Any]:
        with self.connect() as db:
            rows = db.execute("SELECT * FROM blacklist WHERE active=1 ORDER BY id DESC LIMIT 500").fetchall()
            blocked = int(db.execute("SELECT COUNT(*) AS n FROM events WHERE kind='blocked_download' "
                                     "AND ts>=?", (time.time() - 86400,)).fetchone()["n"])
        result = []
        for row in rows:
            value = str(row["value"])
            result.append({"id": int(row["id"]), "kind": str(row["kind"]), "value": value,
                           "note": str(row["note"]), "created": utc_iso(row["created"]),
                           "createdBy": str(row["created_by"])})
        return {"ok": True, "rows": result, "total": len(result), "blocked24h": blocked}

    def blacklist_summary(self) -> dict[str, int]:
        with self.connect() as db:
            rows = db.execute("SELECT kind,COUNT(*) AS n FROM blacklist WHERE active=1 GROUP BY kind").fetchall()
        counts = {row["kind"]: int(row["n"]) for row in rows}
        return {"ip": counts.get("ip", 0), "client": counts.get("client", 0),
                "total": counts.get("ip", 0) + counts.get("client", 0)}

    def add_blacklist(self, kind: str, value: str, note: str = "", admin: str = "", ip: str = "") -> dict[str, Any]:
        normalized = self._normalize_block_value(kind, value)
        note = sanitize_text(str(note or ""))[:200]
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            db.execute("INSERT INTO blacklist(kind,value,note,created,created_by,active) VALUES(?,?,?,?,?,1) "
                       "ON CONFLICT(kind,value) DO UPDATE SET note=excluded.note,active=1,created=excluded.created,"
                       "created_by=excluded.created_by",
                       (kind, normalized, note, time.time(), admin))
            db.execute("COMMIT")
        self.event("blacklist_add", ip, path="/admin/api/blacklist", detail={"kind": kind, "value": normalized, "admin": admin})
        return {"ok": True, "kind": kind, "value": normalized}

    def remove_blacklist(self, ident: int, admin: str = "", ip: str = "") -> dict[str, Any]:
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT kind,value FROM blacklist WHERE id=? AND active=1", (int(ident),)).fetchone()
            if row is None:
                db.execute("COMMIT")
                raise HTTPFailure(404, "blacklist entry not found", "not_found")
            db.execute("UPDATE blacklist SET active=0 WHERE id=?", (int(ident),))
            db.execute("COMMIT")
        self.event("blacklist_remove", ip, path="/admin/api/blacklist/remove",
                   detail={"kind": row["kind"], "value": row["value"], "admin": admin})
        return {"ok": True}

    def cleanup(self, now: float | None = None) -> dict[str, int]:
        timestamp = time.time() if now is None else now
        cutoff = timestamp - self.config.retention_days * 86400
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            deleted = db.execute("DELETE FROM events WHERE ts<?", (cutoff,)).rowcount
            db.execute("DELETE FROM sessions WHERE expires<?", (timestamp,))
            self._expire_reservations(db, timestamp)
            db.execute("COMMIT")
        return {"deletedEvents": deleted}

    def summary(self) -> dict[str, Any]:
        self.cleanup()
        month = month_for()
        with self.connect() as db:
            traffic = db.execute("SELECT * FROM traffic WHERE month=?", (month,)).fetchone()
            reserved = db.execute("SELECT COALESCE(SUM(expected),0) AS n,COUNT(*) AS c,"
                                  "COALESCE(SUM(CASE WHEN object LIKE 'api:%' THEN expected ELSE 0 END),0) AS api "
                                  "FROM reservations WHERE state='active' AND month=?", (month,)).fetchone()
            versions = db.execute("SELECT version,COUNT(*) AS n FROM events WHERE version<>'' GROUP BY version ORDER BY n DESC LIMIT 30").fetchall()
            clients = db.execute("SELECT client,COUNT(*) AS n FROM events WHERE client<>'' GROUP BY client ORDER BY n DESC LIMIT 50").fetchall()
            ips = db.execute("SELECT ip,COUNT(*) AS n FROM events WHERE ip<>'' GROUP BY ip ORDER BY n DESC LIMIT 50").fetchall()
            external = db.execute("SELECT json_extract(detail,'$.target') AS target,COUNT(*) AS n FROM events WHERE kind='external_click' GROUP BY target").fetchall()
            errors = db.execute("SELECT COUNT(*) AS n FROM events WHERE status>=400 AND ts>=?", (time.time() - 86400,)).fetchone()
            feedback = db.execute("SELECT COUNT(*) AS n FROM feedback").fetchone()
            recent = db.execute("SELECT * FROM events ORDER BY id DESC LIMIT 30").fetchall()
        object_used = self.object_bytes()
        feedback_used = self.feedback_bytes()
        object_month = int(traffic["object_bytes"]) if traffic else 0
        api_month = int(traffic["api_bytes"]) if traffic else 0
        return {
            "ok": True, "time": utc_iso(), "month": month, "timezone": "Asia/Shanghai",
            "platforms": {"windows": "enabled", "android": "disabled", "ios": "disabled"},
            "traffic": {
                "objectBytes": object_month, "objectReserved": int(reserved["n"]) - int(reserved["api"]),
                "apiReserved": int(reserved["api"]),
                "apiBytes": api_month,
                **self._traffic_quota_fields(object_month, api_month, int(reserved["n"])),
                "retryAfter": seconds_to_next_month(),
            },
            "storage": {**self.platform_storage(), "objects": object_used, "feedback": feedback_used,
                        "total": self.storage_bytes(), "updateTree": self.update_disk_bytes(),
                        "limit": self.config.storage_bytes,
                        "updateLimit": self.config.update_storage_bytes,
                        "feedbackLimit": self.config.feedback_storage_bytes},
            "reservations": {"active": int(reserved["c"]), "bytes": int(reserved["n"])},
            "feedback": {"count": int(feedback["n"]), "privateAttachments": True},
            "feedbackRecent": self.feedback_recent(),
            "versions": [{"version": row["version"], "count": row["n"]} for row in versions],
            "clients": [{"id": row["client"], "count": row["n"]} for row in clients],
            "ips": [{"ip": mask_ip(row["ip"]), "count": row["n"]} for row in ips],
            "externalClicks": {row["target"] or "unknown": row["n"] for row in external},
            "errors24h": int(errors["n"]),
            "recentEvents": [self.public_event(row) for row in recent],
        }

    @staticmethod
    def public_event(row: sqlite3.Row) -> dict[str, Any]:
        return {"id": row["id"], "ts": row["ts"], "time": utc_iso(row["ts"]), "kind": row["kind"],
                "ip": mask_ip(row["ip"]), "client": row["client"], "version": row["version"],
                "path": row["path"], "status": row["status"], "bytes": row["bytes"],
                "detail": json.loads(row["detail"])}

    def events(self, limit: int = 100) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute("SELECT * FROM events ORDER BY id DESC LIMIT ?", (limit,)).fetchall()
        return [self.public_event(row) for row in rows]

    def reveal_ip(self, event_id: int, admin_ip: str) -> str:
        with self.connect() as db:
            row = db.execute("SELECT ip FROM events WHERE id=?", (event_id,)).fetchone()
        if row is None:
            raise HTTPFailure(404, "event not found", "not_found")
        self.event("audit", admin_ip, path="/admin/api/reveal-ip", detail={"revealedEvent": event_id})
        return row["ip"]

    def export_csv(self) -> bytes:
        output = io.StringIO()
        writer = csv.writer(output, lineterminator="\r\n")
        writer.writerow(("id", "time", "kind", "platform", "masked_ip", "client_id", "version", "path", "status", "bytes", "detail"))
        with self.connect() as db:
            rows = db.execute("SELECT * FROM events ORDER BY id DESC LIMIT 5000").fetchall()
        for row in rows:
            values = (row["id"], utc_iso(row["ts"]), row["kind"], row["platform"] or "windows",
                      mask_ip(row["ip"]), row["client"], row["version"], row["path"], row["status"],
                      row["bytes"], row["detail"])
            writer.writerow("'" + str(value) if isinstance(value, str) and value.startswith(("=", "+", "-", "@")) else value for value in values)
        return output.getvalue().encode("utf-8-sig")


class RLPlayerServer(ThreadingHTTPServer):
    daemon_threads = False
    block_on_close = True
    allow_reuse_address = True

    def __init__(self, address: tuple[str, int], store: Store):
        self.store = store
        self.rate_limiter = RateLimiter()
        super().__init__(address, RLPlayerHandler)


class RLPlayerHandler(BaseHTTPRequestHandler):
    server: RLPlayerServer
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args: Any) -> None:
        return

    @property
    def store(self) -> Store:
        return self.server.store

    @property
    def ip(self) -> str:
        peer = self.client_address[0]
        if peer in ("127.0.0.1", "::1"):
            forwarded = self.headers.get("X-Real-IP", "")
            try:
                return str(ipaddress.ip_address(forwarded))
            except ValueError:
                pass
        return peer

    def do_GET(self) -> None:
        self._run("GET")

    def do_POST(self) -> None:
        self._run("POST")

    def do_PUT(self) -> None:
        self._run("PUT")

    def do_HEAD(self) -> None:
        self._run("HEAD")

    def _run(self, method: str) -> None:
        try:
            self._dispatch(method)
        except HTTPFailure as failure:
            self._json(failure.status, {"ok": False, "error": failure.message, "code": failure.code},
                       retry=failure.retry, head=method == "HEAD")
        except (BrokenPipeError, ConnectionResetError):
            return
        except Exception:
            self._json(500, {"ok": False, "error": "internal server error", "code": "internal_error"}, head=method == "HEAD")
        finally:
            self._drain_body()

    def _drain_body(self) -> None:
        """HTTP/1.1 keep-alive：没被读走的请求体必须丢弃，否则下一个请求会被解析成垃圾。"""
        if getattr(self, "_body_consumed", False):
            return
        try:
            remaining = int(self.headers.get("Content-Length") or 0)
        except (TypeError, ValueError):
            remaining = 0
        if remaining <= 0:
            return
        if remaining > 1024 * 1024:
            self.close_connection = True
            return
        try:
            while remaining > 0:
                chunk = self.rfile.read(min(remaining, 1 << 16))
                if not chunk:
                    break
                remaining -= len(chunk)
        except (OSError, ValueError):
            self.close_connection = True

    def _dispatch(self, method: str) -> None:
        parsed = urllib.parse.urlsplit(self.path)
        path = parsed.path
        public_api = path.startswith("/api/") and path != "/api/v1/health"
        if public_api:
            self.store.api_quota_check()
        if path == "/api/v1/health" and method in ("GET", "HEAD"):
            self._json(200, {"ok": True, "service": "rlplayer", "platform": "windows"}, head=method == "HEAD")
            return
        match = re.fullmatch(r"/api/v1/feedback/(windows|macos)", path)
        if match and method == "POST":
            retry = self.server.rate_limiter.check("feedback", self.ip, self.store.config.feedback_rate,
                                                   self.store.config.feedback_rate_window)
            if retry:
                raise HTTPFailure(429, "feedback rate limit exceeded", "rate_limited", retry)
            status, result = self.store.submit_feedback(
                self._read_json(), self.ip, platform="mac" if match.group(1) == "macos" else "windows")
            self._json(status, result)
            return
        match = re.fullmatch(r"/api/v1/update/(windows|macos)/latest", path)
        if match and method in ("GET", "HEAD"):
            self._manifest(match.group(1), method == "HEAD")
            return
        match = re.fullmatch(r"/api/v1/update/(windows|macos)/objects/([0-9a-f]{64})\.zip", path)
        if match and method in ("GET", "HEAD"):
            self._object(match.group(2), method == "HEAD", match.group(1))
            return
        match = re.fullmatch(r"/admin/api/publish/macos/objects/([0-9a-f]{64})", path)
        if match and method == "PUT":
            self._publish_macos_object(match.group(1))
            return
        if path == "/admin/api/publish/macos/latest" and method == "PUT":
            self._publish_macos_latest()
            return
        if path == "/admin/api/publish/macos/status" and method in ("GET", "HEAD"):
            self._publish_macos_status()
            return
        if path in ("/api/v1/update/windows/report", "/api/v1/update/macos/report",
                    "/api/v1/events/external") and method == "POST":
            retry = self.server.rate_limiter.check("api-write", self.ip, 30, 60)
            if retry:
                raise HTTPFailure(429, "API write rate limit exceeded", "rate_limited", retry)
        if path in ("/api/v1/update/windows/report", "/api/v1/update/macos/report") and method == "POST":
            self.store.report_update(self._read_json(), self.ip,
                                     platform="mac" if "/macos/" in path else "windows")
            self._json(200, {"ok": True})
            return
        if path == "/api/v1/events/external" and method == "POST":
            payload = self._read_json(4096)
            target = self.store.external_target(payload.get("target"))
            if target not in ("baidu", "github-windows", "github-macos"):
                raise HTTPFailure(400, "target must be baidu, github-windows or github-macos")
            self.store.event("external_click", self.ip, path=path, detail={"target": target},
                             platform="mac" if target == "github-macos" else self._platform())
            self._json(200, {"ok": True})
            return
        if path == "/internal/v1/account" and method == "POST":
            self._account(parsed.query)
            return
        if path == "/admin/login":
            self._login(method)
            return
        if path == "/admin/" and method in ("GET", "HEAD"):
            csrf = self._require_admin()
            self._html(200, admin_ui.render_dashboard(csrf, html.escape(getattr(self, "admin_user", "")),
                                                      html.escape(self.store.admin_version())), head=method == "HEAD")
            return
        if path == "/admin/api/overview" and method in ("GET", "HEAD"):
            self._require_admin()
            self._json(200, self.store.overview(), head=method == "HEAD")
            return
        if path == "/admin/api/downloads" and method in ("GET", "HEAD"):
            self._require_admin()
            query = urllib.parse.parse_qs(parsed.query)
            self._json(200, self.store.download_rows(
                range_name=query.get("range", ["day"])[0], platform=query.get("platform", ["all"])[0],
                dimension=query.get("dimension", ["ip"])[0], query=query.get("q", [""])[0],
                limit=self._int_param(query, "limit", 20, 1, 200),
                offset=self._int_param(query, "offset", 0, 0, 1_000_000),
                sort=query.get("sort", ["requests"])[0]), head=method == "HEAD")
            return
        if path == "/admin/api/regions" and method in ("GET", "HEAD"):
            self._require_admin()
            query = urllib.parse.parse_qs(parsed.query)
            self._json(200, self.store.region_rows(
                range_name=query.get("range", ["day"])[0], platform=query.get("platform", ["all"])[0],
                limit=self._int_param(query, "limit", 20, 1, 100)), head=method == "HEAD")
            return
        if path == "/admin/api/external" and method in ("GET", "HEAD"):
            self._require_admin()
            query = urllib.parse.parse_qs(parsed.query)
            self._json(200, self.store.external_counts(query.get("range", ["day"])[0]), head=method == "HEAD")
            return
        if path == "/admin/api/errors" and method in ("GET", "HEAD"):
            self._require_admin()
            query = urllib.parse.parse_qs(parsed.query)
            self._json(200, self.store.error_rows(self._int_param(query, "hours", 24, 1, 720),
                                                  self._int_param(query, "limit", 50, 1, 200)), head=method == "HEAD")
            return
        if path == "/admin/api/feedback" and method in ("GET", "HEAD"):
            self._require_admin()
            query = urllib.parse.parse_qs(parsed.query)
            self._json(200, self.store.feedback_page(query.get("range", ["all"])[0],
                                                     self._int_param(query, "limit", 100, 1, 200),
                                                     self._int_param(query, "offset", 0, 0, 1_000_000),
                                                     query.get("platform", ["all"])[0]),
                       head=method == "HEAD")
            return
        if path == "/admin/api/blacklist" and method in ("GET", "HEAD"):
            self._require_admin()
            self._json(200, self.store.blacklist_rows(), head=method == "HEAD")
            return
        if path == "/admin/api/blacklist" and method == "POST":
            self._require_admin(check_csrf=True)
            payload = self._read_json(4096)
            try:
                result = self.store.add_blacklist(str(payload.get("kind") or ""), str(payload.get("value") or ""),
                                                 str(payload.get("note") or ""), getattr(self, "admin_user", ""), self.ip)
            except ValueError as error:
                raise HTTPFailure(400, str(error)) from None
            self._json(200, result)
            return
        if path == "/admin/api/blacklist/remove" and method == "POST":
            self._require_admin(check_csrf=True)
            payload = self._read_json(4096)
            if not isinstance(payload.get("id"), int):
                raise HTTPFailure(400, "integer blacklist id required")
            self._json(200, self.store.remove_blacklist(payload["id"], getattr(self, "admin_user", ""), self.ip))
            return
        if path == "/admin/api/users" and method in ("GET", "HEAD"):
            self._require_admin()
            self._json(200, {"ok": True, "current": getattr(self, "admin_user", ""), "rows": self.store.users()},
                       head=method == "HEAD")
            return
        if path == "/admin/api/password" and method == "POST":
            self._require_admin(check_csrf=True)
            payload = self._read_json(4096)
            current, new = str(payload.get("current") or ""), str(payload.get("next") or "")
            if len(new) < 8 or len(new) > 200:
                raise HTTPFailure(400, "新密码需要 8~200 个字符")
            if not self.store.change_password(getattr(self, "admin_user", ""), current, new,
                                             keep_token=self._cookie()):
                raise HTTPFailure(403, "当前密码不正确", "forbidden")
            self.store.event("admin_password_changed", self.ip, path="/admin/api/password",
                             detail={"admin": getattr(self, "admin_user", "")})
            self._json(200, {"ok": True})
            return
        if path == "/admin/api/summary" and method in ("GET", "HEAD"):
            self._require_admin()
            self._json(200, self.store.summary(), head=method == "HEAD")
            return
        if path == "/admin/api/events" and method in ("GET", "HEAD"):
            self._require_admin()
            query = urllib.parse.parse_qs(parsed.query)
            self._json(200, {"ok": True, "events": self.store.events(self._int_param(query, "limit", 100, 1, 200))},
                       head=method == "HEAD")
            return
        if path == "/admin/api/feedback/detail" and method in ("GET", "HEAD"):
            self._require_admin()
            query = urllib.parse.parse_qs(parsed.query)
            self._json(200, self.store.feedback_detail(query.get("id", [""])[0]), head=method == "HEAD")
            return
        if path == "/admin/api/feedback/attachment" and method in ("GET", "HEAD"):
            self._require_admin()
            query = urllib.parse.parse_qs(parsed.query)
            ident = query.get("id", [""])[0]
            attachment = self.store.feedback_attachment(ident)
            self._bytes(200, attachment.read_bytes(), "application/json; charset=utf-8",
                        {"Content-Disposition": "attachment; filename=" + ident + ".json"}, head=method == "HEAD")
            return
        if path == "/admin/api/export.csv" and method in ("GET", "HEAD"):
            self._require_admin()
            data = self.store.export_csv()
            self._bytes(200, data, "text/csv; charset=utf-8",
                        {"Content-Disposition": "attachment; filename=rlplayer-events.csv"}, head=method == "HEAD")
            return
        if path == "/admin/api/reveal-ip" and method == "POST":
            self._require_admin(check_csrf=True)
            payload = self._read_json(4096)
            if not isinstance(payload.get("id"), int):
                raise HTTPFailure(400, "integer event id required")
            ip = self.store.reveal_ip(payload["id"], self.ip)
            self._json(200, {"ok": True, "ip": ip})
            return
        if path == "/admin/api/logout" and method == "POST":
            token = self._require_admin(check_csrf=True)
            self.store.logout(self._cookie())
            self._json(200, {"ok": True}, headers={"Set-Cookie": self._cookie_header("", 0)})
            return
        raise HTTPFailure(404, "not found", "not_found")

    def _read_json(self, maximum: int = MAX_REQUEST) -> dict[str, Any]:
        content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
        if content_type != "application/json":
            raise HTTPFailure(415, "application/json required", "unsupported_media_type")
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            raise HTTPFailure(411, "valid Content-Length required", "length_required") from None
        if length < 0 or length > maximum:
            self.close_connection = True
            raise HTTPFailure(413, "request body too large", "request_too_large", 60)
        raw = self.rfile.read(length)
        self._body_consumed = True
        try:
            value = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise HTTPFailure(400, "invalid UTF-8 JSON", "invalid_json") from None
        if not isinstance(value, dict):
            raise HTTPFailure(400, "JSON object required", "invalid_json")
        return value

    @staticmethod
    def _int_param(query: dict[str, list[str]], key: str, default: int, low: int, high: int) -> int:
        try:
            value = int(query.get(key, [str(default)])[0])
        except (TypeError, ValueError):
            raise HTTPFailure(400, "invalid %s" % key) from None
        return max(low, min(high, value))

    def _platform(self) -> str:
        value = (self.headers.get("X-RLPlayer-Platform", "") or "").strip().lower()
        if value in ("mac", "macos", "darwin", "osx"):
            return "mac"
        if value in ("windows", "win"):
            return "windows"
        agent = (self.headers.get("User-Agent", "") or "").lower()
        if "macintosh" in agent or "darwin" in agent or "mac os" in agent:
            return "mac"
        return "windows"

    def _client_raw(self) -> str:
        value = self.headers.get("X-RLPlayer-Client", "") or ""
        if not value:
            query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
            value = query.get("client", [""])[0]
        value = str(value).strip()
        return value if re.fullmatch(r"[A-Za-z0-9._:-]{1,128}", value) else ""

    def _client_hash(self) -> str:
        raw = self._client_raw()
        return self.store.client_id(raw) if raw else ""

    def _version_from_ua(self) -> str:
        match = re.search(r"RLPlayer/([0-9][0-9A-Za-z.\-]*)", self.headers.get("User-Agent", "") or "")
        return match.group(1) if match else ""

    def _reject_blocked(self, route: str, platform: str = "") -> None:
        client = self._client_hash()
        if not self.store.is_blocked(self.ip, client):
            return
        platform = platform or self._platform()
        self.store.event("blocked_download", self.ip, client=client, path=self.path, status=429,
                         detail={"reason": "blacklist", "route": route}, platform=platform)
        raise HTTPFailure(429, "增量下载已暂停，请使用百度网盘或 GitHub 下载完整版",
                          "quota_exceeded", self.store.config.blocked_retry_seconds)

    def _manifest(self, platform: str, head: bool) -> None:
        self._reject_blocked("latest", platform)
        macos = platform == "macos"
        source = self.store.mac_latest if macos else self.store.latest
        expected_platform = "macos" if macos else "windows"
        label = "macOS" if macos else "Windows"
        try:
            data = source.read_bytes()
            if len(data) > 4 * 1024 * 1024:
                raise ValueError("latest too large")
            envelope = json.loads(data)
            if set(envelope) != {"alg", "kid", "payload", "signature"} or envelope["alg"] != "PS256":
                raise ValueError("invalid envelope")
            if not isinstance(envelope["kid"], str) or not envelope["kid"]:
                raise ValueError("invalid kid")
            payload = json.loads(base64.urlsafe_b64decode(envelope["payload"] + "=" * (-len(envelope["payload"]) % 4)))
            if not isinstance(payload, dict) or payload.get("platform") != expected_platform:
                raise ValueError("wrong platform")
            if payload.get("install_mode") not in ("incremental", "manual"):
                raise ValueError("invalid install mode")
        except FileNotFoundError:
            raise HTTPFailure(404, "no %s release published" % label, "not_found") from None
        except (ValueError, KeyError, TypeError, json.JSONDecodeError, base64.binascii.Error):
            raise HTTPFailure(503, "signed release metadata unavailable", "release_unavailable") from None
        etag = '"' + hashlib.sha256(data).hexdigest() + '"'
        if self.headers.get("If-None-Match") == etag:
            self.send_response(304)
            self.send_header("ETag", etag)
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self._bytes(200, data, "application/json; charset=utf-8", {"ETag": etag, "Cache-Control": "no-cache"}, head=head)

    def _object(self, object_id: str, head: bool, platform: str = "windows") -> None:
        platform = "mac" if platform in ("mac", "macos") else "windows"
        self._reject_blocked("object", platform)
        directory = self.store.mac_objects if platform == "mac" else self.store.objects
        path = directory / (object_id + ".zip")
        try:
            size = path.stat().st_size
            if not path.is_file():
                raise FileNotFoundError
        except FileNotFoundError:
            raise HTTPFailure(404, "incremental object not found", "not_found") from None
        etag = '"' + object_id + '"'
        start, end, partial = 0, size - 1, False
        range_header = self.headers.get("Range")
        if_range = self.headers.get("If-Range")
        if range_header and (not if_range or if_range == etag):
            start, end, partial = self._parse_range(range_header, size)
        amount = max(0, end - start + 1) if size else 0
        reservation = self.store.reserve_object(object_id, amount, platform)
        headers = {
            "ETag": etag, "Accept-Ranges": "bytes", "Content-Disposition": "attachment; filename=" + object_id + ".zip",
            "X-RLPlayer-Reservation": reservation, "X-Content-Type-Options": "nosniff",
        }
        if partial:
            headers["Content-Range"] = f"bytes {start}-{end}/{size}"
        if self.store.config.x_accel:
            headers["X-Accel-Redirect"] = (self.store.config.x_accel_prefix + object_id + ".zip" +
                                           "?rlplayer_reservation=" + reservation)
            self._bytes(200 if not partial else 206, b"", "application/zip", headers, content_length=0, head=True)
            return
        headers["Content-Length"] = str(amount)
        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", "application/zip")
        for key, value in headers.items():
            self.send_header(key, value)
        self.send_header("Cache-Control", "private, no-cache")
        self.end_headers()
        actual = 0
        try:
            if not head:
                with path.open("rb") as handle:
                    handle.seek(start)
                    remaining = amount
                    while remaining:
                        chunk = handle.read(min(256 * 1024, remaining))
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        actual += len(chunk)
                        remaining -= len(chunk)
        except (BrokenPipeError, ConnectionResetError):
            actual = amount
        finally:
            self.store.settle_object(reservation, actual)
            self.store.event("object_download", self.ip, client=self._client_hash(), path=self.path,
                             status=206 if partial else 200, byte_count=actual,
                             detail={"object": object_id, "reservation": reservation},
                             platform=platform)

    @staticmethod
    def _parse_range(value: str, size: int) -> tuple[int, int, bool]:
        match = re.fullmatch(r"bytes=(\d*)-(\d*)", value.strip())
        if not match or size == 0 or (not match.group(1) and not match.group(2)):
            raise HTTPFailure(416, "range not satisfiable", "range_not_satisfiable")
        first, last = match.groups()
        if not first:
            suffix = int(last)
            if suffix == 0:
                raise HTTPFailure(416, "range not satisfiable", "range_not_satisfiable")
            start, end = max(0, size - suffix), size - 1
        else:
            start = int(first)
            end = int(last) if last else size - 1
            if start >= size or end < start:
                raise HTTPFailure(416, "range not satisfiable", "range_not_satisfiable")
            end = min(end, size - 1)
        return start, end, True

    def _account(self, query: str) -> None:
        configured = self.store.config.accounting_token
        if not configured or not hmac.compare_digest(self.headers.get("X-Accounting-Token", ""), configured):
            raise HTTPFailure(403, "accounting authentication failed", "forbidden")
        values = urllib.parse.parse_qs(query)
        try:
            reservation = values["reservation"][0]
            actual = int(values["actual"][0])
            if not re.fullmatch(r"[0-9a-f]{32}", reservation) or actual < 0:
                raise ValueError
        except (KeyError, ValueError, IndexError):
            raise HTTPFailure(400, "invalid accounting record") from None
        settled = self.store.settle_object(reservation, actual)
        self._json(200, {"ok": True, "settled": settled})

    def _require_publish_token(self) -> None:
        expected = self.store.config.publish_token_macos
        if not expected:
            raise HTTPFailure(404, "macOS publishing is not configured", "not_found")
        header = self.headers.get("Authorization", "") or ""
        token = header[7:].strip() if header.lower().startswith("bearer ") else ""
        if not token or not hmac.compare_digest(token, expected):
            self.store.event("publish_denied", self.ip, path=self.path, status=403)
            raise HTTPFailure(403, "publishing token required", "forbidden")
        retry = self.server.rate_limiter.check("publish", self.ip, 120, 60)
        if retry:
            raise HTTPFailure(429, "publish rate limit exceeded", "rate_limited", retry)

    def _read_publish_body(self, maximum: int) -> bytes:
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except (TypeError, ValueError):
            length = 0
        if length <= 0 or length > maximum:
            self.close_connection = True
            raise HTTPFailure(413, "invalid body size", "request_too_large", 60)
        raw = self.rfile.read(length)
        self._body_consumed = True
        if len(raw) != length:
            raise HTTPFailure(400, "incomplete request body")
        return raw

    def _publish_macos_object(self, sha: str) -> None:
        self._require_publish_token()
        body = self._read_publish_body(100 * 1024 * 1024)
        result = self.store.store_macos_object(sha, body)
        self.store.event("publish_macos_object", self.ip, path=self.path, byte_count=len(body),
                         detail={"object": sha, "already": bool(result.get("already"))}, platform="mac")
        self._json(200, result)

    def _publish_macos_latest(self) -> None:
        self._require_publish_token()
        body = self._read_publish_body(8 * 1024 * 1024)
        self._json(200, self.store.publish_macos_manifest(body, self.ip))

    def _publish_macos_status(self) -> None:
        self._require_publish_token()
        latest = self.store.macos_latest_payload() or {}
        self._json(200, {
            "ok": True,
            "latest": {"version": latest.get("version", ""), "publishedAt": latest.get("published_at", "")},
            "objects": len(list(self.store.mac_objects.glob("*.zip"))),
            "storage": self.store.platform_storage(),
            "releases": self.store.macos_release_history(),
            "maxObjectBytes": 100 * 1024 * 1024,
        })

    def _login(self, method: str) -> None:
        if method in ("GET", "HEAD"):
            self._html(200, admin_ui.render_login(self.store.login_csrf()), head=method == "HEAD")
            return
        if method != "POST":
            raise HTTPFailure(405, "method not allowed", "method_not_allowed")
        retry = self.server.rate_limiter.check("login", self.ip, self.store.config.login_rate,
                                               self.store.config.login_rate_window)
        if retry:
            raise HTTPFailure(429, "login rate limit exceeded", "rate_limited", retry)
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if length <= 0 or length > 8192:
            raise HTTPFailure(413, "invalid login body", "request_too_large", 60)
        self._body_consumed = True
        form = urllib.parse.parse_qs(self.rfile.read(length).decode("utf-8", errors="replace"), max_num_fields=10)
        username, password, csrf = (form.get(key, [""])[0] for key in ("username", "password", "csrf"))
        if not self.store.check_login_csrf(csrf):
            raise HTTPFailure(403, "login CSRF validation failed", "csrf_failed")
        if not username or not password or not self.store.verify_user(username, password):
            self.store.event("admin_login_failed", self.ip, path="/admin/login", status=401,
                             detail={"username": username[:32]})
            self._html(401, admin_ui.render_login(self.store.login_csrf(), "用户名或密码错误。"))
            return
        token, session_csrf = self.store.create_session(username)
        self.store.event("admin_login", self.ip, path="/admin/login", detail={"admin": username})
        self.send_response(303)
        self.send_header("Location", "/admin/")
        self.send_header("Set-Cookie", self._cookie_header(token, self.store.config.session_ttl))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _cookie(self) -> str:
        cookies = self.headers.get("Cookie", "")
        match = re.search(r"(?:^|;\s*)rlplayer_admin=([^;]+)", cookies)
        return match.group(1) if match else ""

    def _cookie_header(self, token: str, max_age: int) -> str:
        value = f"rlplayer_admin={token}; Path=/admin; HttpOnly; SameSite=Strict; Max-Age={max_age}"
        if self.store.config.secure_cookie:
            value += "; Secure"
        return value

    def _require_admin(self, check_csrf: bool = False) -> str:
        info = self.store.session_info(self._cookie())
        if info is None:
            raise HTTPFailure(401, "administrator authentication required", "unauthorized")
        username, csrf = info
        self.admin_user = username
        if check_csrf and not hmac.compare_digest(csrf, self.headers.get("X-CSRF-Token", "")):
            raise HTTPFailure(403, "CSRF validation failed", "csrf_failed")
        return csrf

    def _json(self, status: int, value: dict[str, Any], *, retry: int | None = None,
              headers: dict[str, str] | None = None, head: bool = False) -> None:
        data = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        extra = dict(headers or {})
        if retry is not None:
            extra["Retry-After"] = str(max(1, int(retry)))
        if status == 416:
            match = re.search(r"/([0-9a-f]{64})\.zip", self.path)
            if match:
                path = self.store.objects / (match.group(1) + ".zip")
                extra["Content-Range"] = f"bytes */{path.stat().st_size if path.exists() else 0}"
        self._bytes(status, data, "application/json; charset=utf-8", extra, head=head)

    def _html(self, status: int, value: str, head: bool = False) -> None:
        self._bytes(status, value.encode("utf-8"), "text/html; charset=utf-8",
                    {"Cache-Control": "no-store", "Content-Security-Policy": "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; frame-ancestors 'none'"},
                    head=head)

    def _bytes(self, status: int, data: bytes, content_type: str, headers: dict[str, str] | None = None,
               *, content_length: int | None = None, head: bool = False) -> None:
        public_api = self.path.startswith("/api/")
        reservation = ""
        if public_api and not head and data:
            try:
                reservation = self.store.reserve_api(len(data), self._platform())
            except HTTPFailure as failure:
                # Even the JSON denial body needs budget. A zero-body 429
                # avoids recursive quota failures and unbounded billed denials.
                status, data, content_length = failure.status, b"", 0
                headers = {"Retry-After": str(max(1, int(failure.retry or 1)))}
        actual = 0
        try:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data) if content_length is None else content_length))
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Referrer-Policy", "no-referrer")
            for key, value in (headers or {}).items():
                self.send_header(key, value)
            self.end_headers()
            if not head and data:
                # sendall cannot report a partial count on disconnect. Retain
                # the conservative reservation in that case, as for objects.
                actual = len(data)
                written = self.wfile.write(data)
                if isinstance(written, int):
                    actual = min(len(data), max(0, written))
        finally:
            if public_api:
                self.store.api_response(self.path, status, actual, self.ip,
                                        version=self._version_from_ua(), client=self._client_hash(),
                                        platform=self._platform(), reservation=reservation)


def make_server(root: str | os.PathLike[str], config: Config | dict[str, Any] | None = None,
                host: str = "127.0.0.1", port: int = 0) -> RLPlayerServer:
    resolved = config if isinstance(config, Config) else Config.load(config)
    store = Store(root, resolved)
    return RLPlayerServer((host, port), store)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="RLPlayer Windows feedback, incremental update and monitoring service")
    parser.add_argument("command", choices=("serve", "set-user", "users", "init-admin"), nargs="?", default="serve")
    parser.add_argument("--username")
    parser.add_argument("--password")
    parser.add_argument("--data-dir", default=str(Path.cwd() / "rlplayer-data"))
    parser.add_argument("--config", help="JSON configuration file; accounting_token may instead come from RLPLAYER_ACCOUNTING_TOKEN")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--production", action="store_true", help="enable Secure cookies and X-Accel-Redirect")
    args = parser.parse_args(argv)
    values: dict[str, Any] = {}
    if args.config:
        values = json.loads(Path(args.config).read_text(encoding="utf-8"))
        if not isinstance(values, dict):
            parser.error("configuration must be a JSON object")
    if args.production:
        values.update(secure_cookie=True, x_accel=True)
    config = Config.load(values)
    store = Store(args.data_dir, config)
    if args.command in ("set-user", "init-admin"):
        username = (args.username or "").strip()
        if not username:
            parser.error("--username is required")
        password = args.password
        if password == "-":
            import sys
            password = sys.stdin.readline().rstrip("\r\n")
        if not password:
            import getpass
            password = getpass.getpass("New password for %s: " % username)
        try:
            store.set_user(username, password)
        except ValueError as error:
            parser.error(str(error))
        print("administrator %s saved (PBKDF2-SHA256, %d users total)" % (username, store.user_count()))
        return 0
    if args.command == "users":
        for item in store.users():
            print("%s\t%s\t%s" % (item["username"], item["role"], item["created"]))
        return 0
    if store.user_count() == 0:
        print("WARNING: no administrator account yet; run: rlplayer_service.py set-user --username NAME --password PASS",
              flush=True)
    if config.x_accel and not config.accounting_token:
        parser.error("X-Accel production mode requires RLPLAYER_ACCOUNTING_TOKEN or accounting_token")
    server = RLPlayerServer((args.host, args.port), store)
    print(f"RLPlayer service listening on http://{args.host}:{args.port} data={store.root}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
