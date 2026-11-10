from __future__ import annotations

import base64
import concurrent.futures
import hashlib
import http.client
import io
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import sqlite3
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-server-service"
BUILD.mkdir(exist_ok=True)
sys.path.insert(0, str(ROOT / "server"))

from rlplayer_service import Config, Store, atomic_write, make_server, month_for  # noqa: E402


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def zip_bytes(name: str = "RLPlayer.exe", data: bytes = b"windows incremental payload") -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
        archive.writestr(info, data)
    return output.getvalue()


class ServiceCase(unittest.TestCase):
    config: dict = {}

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="http-", dir=BUILD)
        self.root = Path(self.temporary.name)
        values = {"feedback_rate": 100, "login_rate": 100, **self.config}
        self.server = make_server(self.root, values)
        self.store: Store = self.server.store
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        self.temporary.cleanup()

    def request(self, method: str, path: str, body: bytes | None = None,
                headers: dict[str, str] | None = None) -> tuple[int, dict[str, str], bytes]:
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=20)
        try:
            try:
                connection.request(method, path, body=body, headers=headers or {})
            except (BrokenPipeError, ConnectionResetError):
                # An oversized request may be rejected before the client finishes its body.
                # Reading the response still proves that the server returned the required error.
                pass
            response = connection.getresponse()
            data = response.read()
            return response.status, {key.lower(): value for key, value in response.getheaders()}, data
        finally:
            connection.close()

    def json_request(self, method: str, path: str, value: dict,
                     headers: dict[str, str] | None = None) -> tuple[int, dict, dict]:
        merged = {"Content-Type": "application/json", **(headers or {})}
        status, response_headers, body = self.request(method, path, json.dumps(value).encode(), merged)
        return status, response_headers, json.loads(body)

    def feedback(self, key: str | None = None, text: str = "playback problem",
                 diag: bytes = b'{"message":"diagnostic"}') -> dict:
        return {"text": text, "version": "3.2.2", "os": "Windows 11", "diag": base64.b64encode(diag).decode(),
                "client": "anonymous-installation-123", "idempotency_key": key or secrets.token_hex(12)}

    def put_latest(self, payload: dict | None = None) -> str:
        value = payload or {"platform": "windows", "version": "3.2.2", "install_mode": "incremental", "files": []}
        encoded = b64url(json.dumps(value, separators=(",", ":")).encode())
        envelope = {"alg": "PS256", "kid": "test-key", "payload": encoded, "signature": b64url(b"signature")}
        raw = json.dumps(envelope, separators=(",", ":")).encode()
        atomic_write(self.store.latest, raw)
        return '"' + hashlib.sha256(raw).hexdigest() + '"'

    def put_object(self, data: bytes | None = None) -> tuple[str, bytes]:
        content = zip_bytes() if data is None else data
        object_id = hashlib.sha256(b"raw Windows executable").hexdigest()
        (self.store.objects / (object_id + ".zip")).write_bytes(content)
        return object_id, content


class FeedbackHTTPTests(ServiceCase):
    def test_health_feedback_idempotency_and_private_redaction(self) -> None:
        status, _, health = self.request("GET", "/api/v1/health")
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(health)["ok"])
        secret = "supersecret-" + secrets.token_hex(8)
        request = self.feedback(key="feedback-idempotency-001", text=f"password={secret} user@example.com",
                                diag=json.dumps({"password": secret, "email": "user@example.com"}).encode())
        status, _, first = self.json_request("POST", "/api/v1/feedback/windows", request)
        self.assertEqual(status, 201)
        self.assertTrue(first["ok"] and first["diagStored"])
        status, _, second = self.json_request("POST", "/api/v1/feedback/windows", request)
        self.assertEqual(status, 200)
        self.assertEqual(second["id"], first["id"])
        self.assertTrue(second["diagStored"])
        with self.store.connect() as db:
            row = db.execute("SELECT * FROM feedback WHERE id=?", (first["id"],)).fetchone()
        self.assertNotIn(secret, row["text"])
        self.assertNotIn("user@example.com", row["text"])
        self.assertEqual(len(row["client"]), 20)
        self.assertNotEqual(row["client"], request["client"])
        attachment = self.store.feedback_dir / row["diag_path"]
        private = attachment.read_bytes()
        self.assertNotIn(secret.encode(), private)
        self.assertNotIn(b"user@example.com", private)
        self.assertIn(b"REDACTED", private)
        status, _, body = self.request("GET", "/" + str(attachment.relative_to(self.root)).replace(os.sep, "/"))
        self.assertEqual(status, 404)
        self.assertNotIn(secret.encode(), body)

    def test_feedback_rate_limit_and_retry_after(self) -> None:
        self.store.config.feedback_rate = 5
        statuses = []
        for index in range(6):
            status, headers, body = self.json_request("POST", "/api/v1/feedback/windows",
                                                      self.feedback(key=f"rate-limit-key-{index:03d}", diag=b""))
            statuses.append(status)
        self.assertEqual(statuses[:5], [201] * 5)
        self.assertEqual(statuses[5], 429)
        self.assertEqual(body["code"], "rate_limited")
        self.assertGreaterEqual(int(headers["retry-after"]), 1)
        status, _, _ = self.json_request("POST", "/api/v1/feedback/windows",
                                         self.feedback(key="rate-limit-other-ip", diag=b""),
                                         {"X-Real-IP": "203.0.113.25"})
        self.assertEqual(status, 201)

    def test_request_text_diag_and_body_limits(self) -> None:
        status, headers, body = self.json_request("POST", "/api/v1/feedback/windows",
                                                  self.feedback(text="汉" * 6000, diag=b""))
        self.assertEqual(status, 413)
        self.assertEqual(body["code"], "request_too_large")
        self.assertIn("retry-after", headers)
        large_diag = b'["' + b"a" * (8 * 1024**2 - 3) + b'"]'
        self.assertEqual(len(large_diag), 8 * 1024**2 + 1)
        status, _, body = self.json_request("POST", "/api/v1/feedback/windows", self.feedback(diag=large_diag))
        self.assertEqual(status, 413)
        self.assertEqual(body["code"], "request_too_large")
        status, headers, body = self.request("POST", "/api/v1/feedback/windows", b" " * (12 * 1024**2 + 1),
                                             {"Content-Type": "application/json"})
        self.assertEqual(status, 413)
        self.assertIn("retry-after", headers)
        self.assertEqual(json.loads(body)["code"], "request_too_large")

    def test_attachment_quota_accepts_text_and_explicitly_drops_diag(self) -> None:
        self.store.config.feedback_storage_bytes = 1
        status, _, body = self.json_request("POST", "/api/v1/feedback/windows", self.feedback())
        self.assertEqual(status, 201)
        self.assertTrue(body["ok"])
        self.assertFalse(body["diagStored"])
        self.assertEqual(body["diagReason"], "feedback_storage_quota")
        self.assertEqual(list(self.store.feedback_dir.iterdir()), [])

    def test_invalid_diag_never_written_or_unpacked(self) -> None:
        status, _, body = self.json_request("POST", "/api/v1/feedback/windows", self.feedback(diag=b"MZ executable"))
        self.assertEqual(status, 422)
        self.assertEqual(body["code"], "unsupported_diagnostic")
        self.assertEqual(list(self.store.feedback_dir.iterdir()), [])
        status, _, body = self.json_request("POST", "/api/v1/feedback/windows",
                                            self.feedback(diag=b"PK\x03\x04not-a-real-zip"))
        self.assertEqual(status, 422)
        self.assertEqual(body["code"], "unsupported_diagnostic")
        self.assertEqual(list(self.store.feedback_dir.iterdir()), [])


class UpdateHTTPTests(ServiceCase):
    def test_latest_strong_etag_and_304(self) -> None:
        etag = self.put_latest()
        status, headers, body = self.request("GET", "/api/v1/update/windows/latest")
        self.assertEqual(status, 200)
        self.assertEqual(headers["etag"], etag)
        envelope = json.loads(body)
        self.assertEqual(set(envelope), {"alg", "kid", "payload", "signature"})
        self.assertEqual(envelope["alg"], "PS256")
        status, headers, body = self.request("GET", "/api/v1/update/windows/latest", headers={"If-None-Match": etag})
        self.assertEqual(status, 304)
        self.assertEqual(body, b"")
        self.assertEqual(headers["etag"], etag)

    def test_object_200_single_range_416_and_if_range(self) -> None:
        object_id, content = self.put_object()
        path = f"/api/v1/update/windows/objects/{object_id}.zip"
        status, headers, body = self.request("GET", path)
        self.assertEqual(status, 200)
        self.assertEqual(body, content)
        self.assertEqual(headers["etag"], f'"{object_id}"')
        self.assertEqual(headers["accept-ranges"], "bytes")
        status, headers, body = self.request("GET", path, headers={"Range": "bytes=2-17"})
        self.assertEqual(status, 206)
        self.assertEqual(body, content[2:18])
        self.assertEqual(headers["content-range"], f"bytes 2-17/{len(content)}")
        status, _, body = self.request("GET", path, headers={"Range": "bytes=-9"})
        self.assertEqual(status, 206)
        self.assertEqual(body, content[-9:])
        status, headers, body = self.request("GET", path, headers={"Range": f"bytes={len(content)}-999"})
        self.assertEqual(status, 416)
        self.assertEqual(headers["content-range"], f"bytes */{len(content)}")
        self.assertEqual(json.loads(body)["code"], "range_not_satisfiable")
        status, _, body = self.request("GET", path, headers={"Range": "bytes=2-17", "If-Range": f'"{object_id}"'})
        self.assertEqual(status, 206)
        self.assertEqual(body, content[2:18])
        status, _, body = self.request("GET", path, headers={"Range": "bytes=2-17", "If-Range": '"stale"'})
        self.assertEqual(status, 200)
        self.assertEqual(body, content)
        status, _, body = self.request("GET", path, headers={"Range": "bytes=0-1,4-5"})
        self.assertEqual(status, 416)

    def test_only_incremental_object_route_and_update_report(self) -> None:
        object_id, _ = self.put_object()
        for suffix in ("../latest.json", object_id + ".exe", "A" * 64 + ".zip"):
            status, _, _ = self.request("GET", "/api/v1/update/windows/objects/" + suffix)
            self.assertEqual(status, 404)
        status, _, body = self.json_request("POST", "/api/v1/update/windows/report",
                                            {"from": "3.2.1", "to": "3.2.2", "ok": True, "ms": 1250, "client": "install-id"})
        self.assertEqual(status, 200)
        self.assertTrue(body["ok"])
        with self.store.connect() as db:
            row = db.execute("SELECT * FROM reports").fetchone()
        self.assertEqual((row["from_version"], row["to_version"], row["ok"], row["ms"]), ("3.2.1", "3.2.2", 1, 1250))
        self.assertEqual(len(row["client"]), 20)

    def test_public_write_rate_limit_preserves_database(self) -> None:
        results = [self.json_request("POST", "/api/v1/events/external", {"target": "github"})
                   for _ in range(31)]
        self.assertEqual([item[0] for item in results[:30]], [200] * 30)
        status, headers, body = results[-1]
        self.assertEqual(status, 429)
        self.assertEqual(body["code"], "rate_limited")
        self.assertGreaterEqual(int(headers["retry-after"]), 1)

    def test_concurrent_reservations_cannot_exceed_object_quota(self) -> None:
        object_id, content = self.put_object()
        self.store.config.monthly_object_bytes = len(content)
        path = f"/api/v1/update/windows/objects/{object_id}.zip"
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as executor:
            results = list(executor.map(lambda _: self.request("GET", path), range(12)))
        statuses = [result[0] for result in results]
        self.assertEqual(statuses.count(200), 1, statuses)
        self.assertEqual(statuses.count(429), 11, statuses)
        for status, headers, body in results:
            if status == 429:
                self.assertEqual(json.loads(body)["code"], "quota_exceeded")
                self.assertGreaterEqual(int(headers["retry-after"]), 1)
        with self.store.connect() as db:
            traffic = db.execute("SELECT object_bytes FROM traffic").fetchone()
            active = db.execute("SELECT COUNT(*) AS n FROM reservations WHERE state='active'").fetchone()
        self.assertEqual(traffic["object_bytes"], len(content))
        self.assertEqual(active["n"], 0)

    def test_expired_reservation_is_conservatively_charged(self) -> None:
        self.store.config.monthly_object_bytes = 100
        reservation = self.store.reserve_object("a" * 64, 40)
        with self.store.connect() as db:
            db.execute("UPDATE reservations SET created=? WHERE id=?", (time.time() - 1000, reservation))
        with self.assertRaisesRegex(Exception, "monthly object quota"):
            self.store.reserve_object("b" * 64, 70)
        with self.store.connect() as db:
            traffic = db.execute("SELECT object_bytes FROM traffic").fetchone()
            row = db.execute("SELECT state,actual FROM reservations WHERE id=?", (reservation,)).fetchone()
        self.assertEqual(traffic["object_bytes"], 40)
        self.assertEqual((row["state"], row["actual"]), ("expired", 40))
        self.assertFalse(self.store.settle_object(reservation, 1))

    def test_quota_warning_90_and_api_quota(self) -> None:
        self.store.config.monthly_object_bytes = 100
        self.store.reserve_object("a" * 64, 90)
        summary = self.store.summary()
        self.assertTrue(summary["traffic"]["warning80"])
        self.assertTrue(summary["traffic"]["incrementalOnly90"])
        self.store.config.monthly_api_bytes = 1
        status, _, _ = self.request("GET", "/api/v1/update/windows/latest")
        self.assertEqual(status, 404)
        status, headers, body = self.request("GET", "/api/v1/update/windows/latest")
        self.assertEqual(status, 429)
        self.assertEqual(json.loads(body)["code"], "quota_exceeded")
        self.assertIn("retry-after", headers)
        status, _, _ = self.request("GET", "/api/v1/health")
        self.assertEqual(status, 200)

    def test_x_accel_reservation_and_actual_accounting(self) -> None:
        object_id, content = self.put_object()
        token = secrets.token_hex(24)
        self.store.config.x_accel = True
        self.store.config.accounting_token = token
        path = f"/api/v1/update/windows/objects/{object_id}.zip"
        status, headers, body = self.request("GET", path, headers={"Range": "bytes=3-12"})
        self.assertEqual(status, 206)
        self.assertEqual(body, b"")
        reservation = headers["x-rlplayer-reservation"]
        redirect = urllib.parse.urlsplit(headers["x-accel-redirect"])
        self.assertEqual(redirect.path, "/objects/" + object_id + ".zip")
        self.assertEqual(urllib.parse.parse_qs(redirect.query).get("rlplayer_reservation"), [reservation])
        query = urllib.parse.urlencode({"reservation": reservation, "actual": 10})
        status, _, _ = self.request("POST", "/internal/v1/account?" + query)
        self.assertEqual(status, 403)
        status, _, body = self.json_request("POST", "/internal/v1/account?" + query, {}, {"X-Accounting-Token": token})
        self.assertEqual(status, 200)
        self.assertTrue(body["settled"])
        with self.store.connect() as db:
            row = db.execute("SELECT expected,actual,state FROM reservations WHERE id=?", (reservation,)).fetchone()
            traffic = db.execute("SELECT object_bytes FROM traffic").fetchone()
        self.assertEqual((row["expected"], row["actual"], row["state"]), (10, 10, "settled"))
        self.assertEqual(traffic["object_bytes"], 10)


class AdminHTTPTests(ServiceCase):
    def test_device_download_stats_exclude_unknown_ids_consistently(self) -> None:
        self.store.event("object_download", "192.0.2.1", client="", byte_count=123, platform="windows")
        unknown = self.store.download_rows(dimension="client", platform="windows")
        self.assertEqual(unknown["total"], 0)
        self.assertEqual(unknown["rows"], [])
        self.assertEqual(unknown["totals"], {"requests": 0, "bytes": 0})
        self.store.event("object_download", "192.0.2.2", client="a" * 24, byte_count=50, platform="windows")
        self.store.event("object_download", "192.0.2.3", client="b" * 24, byte_count=75, platform="windows")
        known = self.store.download_rows(dimension="client", platform="windows", limit=1)
        self.assertEqual((known["total"], known["pages"], len(known["rows"])), (2, 2, 1))
        self.assertEqual(known["totals"], {"requests": 2, "bytes": 125})
        ips = self.store.download_rows(dimension="ip", platform="windows")
        self.assertEqual(ips["totals"], {"requests": 3, "bytes": 248})

    config = {"pbkdf2_iterations": 100_000}

    def setUp(self) -> None:
        super().setUp()
        self.store.set_user("huoyanyan", "TEST-ADMIN-PASS-1")
        self.store.set_user("xiaomao", "TEST-ADMIN-PASS-2")

    def login_csrf(self) -> str:
        status, _, body = self.request("GET", "/admin/login")
        self.assertEqual(status, 200)
        match = re.search(rb'name="csrf" value="([^"]+)"', body)
        self.assertIsNotNone(match)
        return match.group(1).decode()

    def post_login(self, username: str, password: str, csrf: str) -> tuple[int, dict[str, str], bytes]:
        body = urllib.parse.urlencode({"username": username, "password": password, "csrf": csrf}).encode()
        return self.request("POST", "/admin/login", body, {"Content-Type": "application/x-www-form-urlencoded"})

    def good_login(self, username: str = "huoyanyan", password: str = "TEST-ADMIN-PASS-1") -> tuple[str, str]:
        status, headers, _ = self.post_login(username, password, self.login_csrf())
        self.assertEqual(status, 303)
        cookie = headers["set-cookie"]
        self.assertIn("HttpOnly", cookie)
        self.assertIn("SameSite=Strict", cookie)
        raw = cookie.split(";", 1)[0].split("=", 1)[1]
        status, _, body = self.request("GET", "/admin/", headers={"Cookie": "rlplayer_admin=" + raw})
        self.assertEqual(status, 200)
        page = body.decode("utf-8")
        self.assertIn("增量更新", page)
        self.assertIn("黑名单", page)
        self.assertIn(username, page)
        self.assertIn("/admin/api/overview", page)
        self.assertNotIn("cdn.", page.lower())
        self.assertNotIn("totp", page.lower())
        csrf = re.search(r'<meta name="csrf" content="([^"]+)"', page).group(1)
        return raw, csrf

    def test_login_two_users_bad_password_csrf_and_unauthorized(self) -> None:
        status, _, _ = self.post_login("huoyanyan", "wrong-password", self.login_csrf())
        self.assertEqual(status, 401)
        status, _, _ = self.post_login("nobody", "TEST-ADMIN-PASS-1", self.login_csrf())
        self.assertEqual(status, 401)
        status, _, body = self.post_login("huoyanyan", "TEST-ADMIN-PASS-1", "invalid")
        self.assertEqual(status, 403)
        self.assertEqual(json.loads(body)["code"], "csrf_failed")
        for path in ("/admin/", "/admin/api/overview", "/admin/api/downloads", "/admin/api/blacklist"):
            status, _, _ = self.request("GET", path)
            self.assertEqual(status, 401, path)
        for username, password in (("huoyanyan", "TEST-ADMIN-PASS-1"), ("xiaomao", "TEST-ADMIN-PASS-2")):
            raw, csrf = self.good_login(username, password)
            auth = {"Cookie": "rlplayer_admin=" + raw}
            status, _, users = self.json_request("GET", "/admin/api/users", {}, auth)
            self.assertEqual(status, 200)
            self.assertEqual(users["current"], username)
            self.assertEqual({item["username"] for item in users["rows"]}, {"huoyanyan", "xiaomao"})
            self.assertTrue(all(item["role"] == "admin" for item in users["rows"]))
            status, _, _ = self.request("POST", "/admin/api/logout", b"", {**auth, "X-CSRF-Token": csrf})
            self.assertEqual(status, 200)
            status, _, _ = self.request("GET", "/admin/api/overview", headers=auth)
            self.assertEqual(status, 401)

    def test_summary_reveal_csv_and_private_attachment(self) -> None:
        self.json_request("POST", "/api/v1/events/external", {"target": "baidu"})
        self.json_request("POST", "/api/v1/events/external", {"target": "github"})
        secret = "synthetic-feedback-secret"
        status, _, ticket = self.json_request("POST", "/api/v1/feedback/windows",
            self.feedback(key="admin-feedback-fixture", text="password=" + secret,
                          diag=json.dumps({"password": secret}).encode()))
        self.assertEqual(status, 201)
        status, _, _ = self.request("GET", "/admin/api/feedback/detail?id=" + ticket["id"])
        self.assertEqual(status, 401)
        raw_cookie, csrf = self.good_login()
        auth = {"Cookie": "rlplayer_admin=" + raw_cookie}
        status, _, summary = self.json_request("GET", "/admin/api/summary", {}, auth)
        self.assertEqual(status, 200)
        self.assertEqual(summary["platforms"], {"windows": "enabled", "android": "disabled", "ios": "disabled"})
        self.assertEqual(summary["externalClicks"], {"baidu": 1, "github-windows": 1})
        self.assertEqual(summary["feedbackRecent"][0]["id"], ticket["id"])
        status, _, detail = self.json_request("GET", "/admin/api/feedback/detail?id=" + ticket["id"], {}, auth)
        self.assertEqual(status, 200)
        self.assertNotIn(secret, detail["text"])
        self.assertIn("REDACTED", detail["text"])
        status, headers, attachment = self.request("GET", "/admin/api/feedback/attachment?id=" + ticket["id"], headers=auth)
        self.assertEqual(status, 200)
        self.assertIn(ticket["id"] + ".json", headers["content-disposition"])
        self.assertNotIn(secret.encode(), attachment)
        self.assertIn(b"REDACTED", attachment)
        serialized = json.dumps(summary)
        self.assertNotIn("127.0.0.1", serialized)
        self.assertIn("127.0.0.*", serialized)
        event_id = next(item["id"] for item in summary["recentEvents"] if item["kind"] == "admin_login")
        reveal = {"Content-Type": "application/json", **auth}
        status, _, body = self.json_request("POST", "/admin/api/reveal-ip", {"id": event_id}, reveal)
        self.assertEqual(status, 403)
        self.assertEqual(body["code"], "csrf_failed")
        status, _, body = self.json_request("POST", "/admin/api/reveal-ip", {"id": event_id},
                                            {**reveal, "X-CSRF-Token": csrf})
        self.assertEqual(status, 200)
        self.assertEqual(body["ip"], "127.0.0.1")
        status, _, events = self.json_request("GET", "/admin/api/events?limit=200", {}, auth)
        self.assertTrue(any(item["kind"] == "audit" and item["detail"]["revealedEvent"] == event_id
                            for item in events["events"]))
        status, headers, csv_data = self.request("GET", "/admin/api/export.csv", headers=auth)
        self.assertEqual(status, 200)
        self.assertIn("text/csv", headers["content-type"])
        self.assertTrue(csv_data.startswith(b"\xef\xbb\xbf"))
        self.assertNotIn(b"127.0.0.1", csv_data)
        self.assertIn(b"127.0.0.*", csv_data)

    def test_overview_and_feedback_page_ranges(self) -> None:
        raw_cookie, _ = self.good_login()
        auth = {"Cookie": "rlplayer_admin=" + raw_cookie}
        self.json_request("POST", "/api/v1/feedback/windows", self.feedback())
        status, _, overview = self.json_request("GET", "/admin/api/overview", {}, auth)
        self.assertEqual(status, 200)
        self.assertGreaterEqual(overview["feedback"]["day"], 1)
        self.assertGreaterEqual(overview["feedback"]["month"], overview["feedback"]["day"])
        self.assertEqual(len(overview["trend"]["days"]), 14)
        self.assertEqual(len(overview["trend"]["objectBytes"]), 14)
        self.assertGreaterEqual(overview["trend"]["feedback"][-1], 1)
        self.assertIn("windows", overview["traffic"])
        self.assertIn("mac", overview["traffic"])
        self.assertGreater(overview["storage"]["total"]["limit"], overview["storage"]["total"]["used"])
        self.assertTrue(overview["incrementalSince"])
        status, _, page = self.json_request("GET", "/admin/api/feedback?range=day", {}, auth)
        self.assertEqual(status, 200)
        self.assertGreaterEqual(page["counts"]["day"], 1)
        self.assertTrue(page["rows"][0]["title"])
        status, _, page_all = self.json_request("GET", "/admin/api/feedback?range=all", {}, auth)
        self.assertGreaterEqual(page_all["total"], 1)

    def test_download_stats_platform_device_and_regions(self) -> None:
        # This HTTP aggregation/serialization test must not depend on the
        # optional production ip2region reader or its 11 MB worldwide database.
        # Only the test transport's loopback address is mapped; this fixture
        # does not validate GeoLookup's real database parsing/classification.
        class LoopbackGeoFixture:
            def __init__(self) -> None:
                self.calls: list[str] = []
                self.closed = False
                self.error = ""

            @property
            def available(self) -> bool:
                return not self.closed

            def lookup(self, ip: str) -> dict:
                if self.closed or ip != "127.0.0.1":
                    raise AssertionError("HTTP geo fixture supports only live loopback lookups")
                self.calls.append(ip)
                return {"country": "Reserved", "province": "", "city": "", "isp": "",
                        "code": "", "reserved": True}

            def close(self) -> None:
                self.closed = True

        original_geo = self.store.geo
        fixture_geo = LoopbackGeoFixture()
        self.store.geo = fixture_geo

        def restore_geo() -> None:
            self.store.geo = original_geo
            fixture_geo.close()

        # unittest cleanup also restores the dependency after an assertion fails.
        self.addCleanup(restore_geo)
        object_id, _ = self.put_object()
        path = "/api/v1/update/windows/objects/" + object_id + ".zip"
        self.assertEqual(self.request("GET", path, headers={"X-RLPlayer-Client": "install-win-0002"})[0], 200)
        content = b"macOS object for platform statistics"
        sha = hashlib.sha256(content).hexdigest()
        (self.store.mac_objects / (sha + ".zip")).write_bytes(MacOSHTTPTests.object_zip(content))
        self.assertEqual(self.request("GET", "/api/v1/update/macos/objects/" + sha + ".zip",
                                      headers={"X-RLPlayer-Client": "install-mac-0001"})[0], 200)
        raw_cookie, _ = self.good_login()
        auth = {"Cookie": "rlplayer_admin=" + raw_cookie}
        status, _, mac = self.json_request("GET", "/admin/api/downloads?range=day&platform=mac", {}, auth)
        self.assertEqual(status, 200)
        self.assertEqual(mac["totals"]["requests"], 1)
        self.assertEqual(mac["rows"][0]["country"], "Reserved")
        status, _, win = self.json_request("GET", "/admin/api/downloads?range=day&platform=windows", {}, auth)
        self.assertEqual(win["totals"]["requests"], 1)
        status, _, devices = self.json_request("GET", "/admin/api/downloads?range=day&dimension=client", {}, auth)
        self.assertEqual(devices["total"], 2)
        hashes = {row["value"] for row in devices["rows"]}
        self.assertIn(self.store.client_id("install-mac-0001"), hashes)
        status, _, regions = self.json_request("GET", "/admin/api/regions?range=day", {}, auth)
        self.assertEqual(status, 200)
        self.assertEqual(regions["rows"][0]["country"], "Reserved")
        status, _, errors = self.json_request("GET", "/admin/api/errors?hours=24", {}, auth)
        self.assertEqual(status, 200)
        self.assertIn("rows", errors)
        status, _, external = self.json_request("GET", "/admin/api/external?range=month", {}, auth)
        self.assertEqual(external["baidu"], 0)
        self.assertGreaterEqual(len(fixture_geo.calls), 3)
        self.assertEqual(set(fixture_geo.calls), {"127.0.0.1"})

    def test_platform_storage_is_separate_and_external_clicks_split(self) -> None:
        raw_cookie, csrf = self.good_login()
        auth = {"Cookie": "rlplayer_admin=" + raw_cookie}
        headers = {"Content-Type": "application/json", **auth, "X-CSRF-Token": csrf}
        status, _, overview = self.json_request("GET", "/admin/api/overview", {}, auth)
        self.assertEqual(status, 200)
        storage = overview["storage"]
        self.assertEqual(storage["windows"]["limit"], 5 * 1024**3)
        self.assertEqual(storage["macos"]["limit"], 5 * 1024**3)
        self.assertEqual(storage["windows"]["used"], 0)
        self.assertEqual(storage["macos"]["used"], 0)
        content = b"macOS storage object"
        sha = hashlib.sha256(content).hexdigest()
        (self.store.mac_objects / (sha + ".zip")).write_bytes(MacOSHTTPTests.object_zip(content))
        status, _, overview = self.json_request("GET", "/admin/api/overview", {}, auth)
        self.assertGreater(overview["storage"]["macos"]["used"], 0)
        self.assertEqual(overview["storage"]["windows"]["used"], 0)
        self.assertEqual(self.json_request("POST", "/api/v1/events/external", {"target": "github"}, headers)[0], 200)
        self.assertEqual(self.json_request("POST", "/api/v1/events/external", {"target": "github-macos"}, headers)[0], 200)
        self.assertEqual(self.json_request("POST", "/api/v1/events/external", {"target": "baidu"}, headers)[0], 200)
        status, _, external = self.json_request("GET", "/admin/api/external?range=month", {}, auth)
        self.assertEqual(external["githubWindows"], 1)
        self.assertEqual(external["githubMacos"], 1)
        self.assertEqual(external["baidu"], 1)
        self.assertEqual(external["total"], 3)
        status, _, overview = self.json_request("GET", "/admin/api/overview", {}, auth)
        self.assertEqual(overview["external"]["githubWindows"], 1)
        self.assertEqual(overview["external"]["githubMacos"], 1)

    def test_feedback_platform_tagging_and_filter(self) -> None:
        windows_payload = self.feedback(key="fb-win-0001", text="Windows 播放问题")
        status, _, windows_ticket = self.json_request("POST", "/api/v1/feedback/windows", windows_payload)
        self.assertEqual(status, 201)
        mac_payload = self.feedback(key="fb-mac-0001", text="macOS 播放问题")
        mac_payload["os"] = "macOS 15 · RLPlayer"
        status, _, mac_ticket = self.json_request("POST", "/api/v1/feedback/macos", mac_payload)
        self.assertEqual(status, 201)
        guessed = self.feedback(key="fb-guess-0001", text="旧客户端没带平台")
        guessed["os"] = "macOS 14 · RLPlayer"
        status, _, guessed_ticket = self.json_request("POST", "/api/v1/feedback/windows", guessed)
        self.assertEqual(status, 201)
        raw_cookie, _ = self.good_login()
        auth = {"Cookie": "rlplayer_admin=" + raw_cookie}
        status, _, detail = self.json_request("GET", "/admin/api/feedback/detail?id=" + mac_ticket["id"], {}, auth)
        self.assertEqual(detail["platform"], "mac")
        status, _, detail = self.json_request("GET", "/admin/api/feedback/detail?id=" + guessed_ticket["id"], {}, auth)
        self.assertEqual(detail["platform"], "mac", "旧客户端 os 写 macOS 时必须归到 mac，不能混进 Windows")
        status, _, mac_only = self.json_request("GET", "/admin/api/feedback?range=all&platform=mac", {}, auth)
        self.assertEqual({row["id"] for row in mac_only["rows"]}, {mac_ticket["id"], guessed_ticket["id"]})
        status, _, win_only = self.json_request("GET", "/admin/api/feedback?range=all&platform=windows", {}, auth)
        self.assertEqual({row["id"] for row in win_only["rows"]}, {windows_ticket["id"]})
        self.assertGreaterEqual(win_only["byPlatform"]["mac"], 2)
        status, headers, csv_data = self.request("GET", "/admin/api/export.csv", headers=auth)
        self.assertIn(b"platform", csv_data.split(b"\r\n")[0])

    def test_macos_upload_respects_its_own_quota(self) -> None:
        self.store.config.publish_token_macos = "test-publish-token"
        self.store.config.macos_update_bytes = 64
        content = b"x" * 200
        sha = hashlib.sha256(content).hexdigest()
        archive = MacOSHTTPTests.object_zip(content)
        status, _headers, body = self.request("PUT", "/admin/api/publish/macos/objects/" + sha, archive,
                                              {"Authorization": "Bearer test-publish-token",
                                               "Content-Type": "application/zip"})
        self.assertEqual(status, 429, body[:120])
        self.assertEqual(self.json_request("GET", "/admin/api/overview", {})[0], 401)

    def test_blacklist_blocks_downloads_but_not_feedback(self) -> None:
        object_id, _ = self.put_object()
        self.put_latest()
        raw_cookie, csrf = self.good_login()
        auth = {"Cookie": "rlplayer_admin=" + raw_cookie}
        headers = {"Content-Type": "application/json", **auth, "X-CSRF-Token": csrf}
        status, _, body = self.json_request("POST", "/admin/api/blacklist",
                                            {"kind": "ip", "value": "127.0.0.1", "note": "test"}, headers)
        self.assertEqual(status, 200)
        self.assertEqual(body["value"], "127.0.0.1")
        status, response_headers, body = self.request("GET", "/api/v1/update/windows/latest")
        self.assertEqual(status, 429)
        self.assertEqual(json.loads(body)["code"], "quota_exceeded")
        self.assertIn("retry-after", response_headers)
        self.assertIn("网盘", json.loads(body)["error"])
        status, _, _ = self.request("GET", "/api/v1/update/windows/objects/" + object_id + ".zip")
        self.assertEqual(status, 429)
        status, _, ticket = self.json_request("POST", "/api/v1/feedback/windows", self.feedback())
        self.assertEqual(status, 201)
        status, _, rows = self.json_request("GET", "/admin/api/blacklist", {}, auth)
        self.assertEqual(rows["total"], 1)
        self.assertEqual(rows["rows"][0]["value"], "127.0.0.1")
        self.assertGreaterEqual(rows["blocked24h"], 2)
        status, _, removed = self.json_request("POST", "/admin/api/blacklist/remove",
                                               {"id": rows["rows"][0]["id"]}, headers)
        self.assertEqual(status, 200)
        status, _, _ = self.request("GET", "/api/v1/update/windows/objects/" + object_id + ".zip")
        self.assertEqual(status, 200)
        status, _, body = self.json_request("POST", "/admin/api/blacklist",
                                            {"kind": "ip", "value": "not-an-ip"}, headers)
        self.assertEqual(status, 400)
        status, _, body = self.json_request("POST", "/admin/api/blacklist",
                                            {"kind": "ip", "value": "203.0.113.0/24"}, headers)
        self.assertEqual(status, 200)
        self.assertEqual(body["value"], "203.0.113.0/24")

    def test_blacklist_device_id_and_wildcards(self) -> None:
        object_id, _ = self.put_object()
        raw_cookie, csrf = self.good_login()
        auth = {"Cookie": "rlplayer_admin=" + raw_cookie}
        headers = {"Content-Type": "application/json", **auth, "X-CSRF-Token": csrf}
        device = self.store.client_id("install-blocked-42")
        status, _, body = self.json_request("POST", "/admin/api/blacklist",
                                            {"kind": "client", "value": device[:12]}, headers)
        self.assertEqual(status, 200)
        status, _, _ = self.request("GET", "/api/v1/update/windows/objects/" + object_id + ".zip",
                                    headers={"X-RLPlayer-Client": "install-blocked-42"})
        self.assertEqual(status, 429)
        status, _, _ = self.request("GET", "/api/v1/update/windows/objects/" + object_id + ".zip",
                                    headers={"X-RLPlayer-Client": "install-allowed-43"})
        self.assertEqual(status, 200)
        status, _, body = self.json_request("POST", "/admin/api/blacklist",
                                            {"kind": "ip", "value": "198.51.100.*"}, headers)
        self.assertEqual(status, 200)
        self.assertTrue(self.store.is_blocked("198.51.100.7", ""))
        self.assertFalse(self.store.is_blocked("198.51.101.7", ""))

    def test_password_change_and_session_expiry(self) -> None:
        raw_cookie, csrf = self.good_login()
        auth = {"Cookie": "rlplayer_admin=" + raw_cookie}
        headers = {"Content-Type": "application/json", **auth, "X-CSRF-Token": csrf}
        status, _, _ = self.json_request("POST", "/admin/api/password",
                                         {"current": "wrong", "next": "anotherpass123"}, headers)
        self.assertEqual(status, 403)
        status, _, _ = self.json_request("POST", "/admin/api/password",
                                         {"current": "TEST-ADMIN-PASS-1", "next": "anotherpass123"}, headers)
        self.assertEqual(status, 200)
        status, _, _ = self.request("GET", "/admin/api/overview", headers=auth)
        self.assertEqual(status, 200)
        status, _, _ = self.post_login("huoyanyan", "TEST-ADMIN-PASS-1", self.login_csrf())
        self.assertEqual(status, 401)
        status, _, _ = self.post_login("huoyanyan", "anotherpass123", self.login_csrf())
        self.assertEqual(status, 303)
        token, csrf2 = self.store.create_session("huoyanyan")
        self.assertEqual(self.store.session(token), csrf2)
        with self.store.connect() as db:
            db.execute("UPDATE sessions SET expires=? WHERE token_hash=?",
                       (time.time() - 1, hashlib.sha256(token.encode()).hexdigest()))
        self.assertIsNone(self.store.session(token))
        status, _, _ = self.request("GET", "/admin/api/overview", headers={"Cookie": "rlplayer_admin=" + token})
        self.assertEqual(status, 401)

    def test_unread_body_does_not_break_keepalive(self) -> None:
        # 未认证 POST 带 body：401 之后同一连接上的下一个请求必须仍是 200，而不是被解析成垃圾请求（501）
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=20)
        try:
            connection.request("POST", "/admin/api/logout", body=b"{}",
                               headers={"Content-Type": "application/json"})
            first = connection.getresponse()
            first.read()
            self.assertEqual(first.status, 401)
            connection.request("GET", "/api/v1/health")
            second = connection.getresponse()
            second.read()
            self.assertEqual(second.status, 200)
        finally:
            connection.close()

    def test_authenticated_logout_with_body_keeps_connection_usable(self) -> None:
        raw_cookie, csrf = self.good_login()
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=20)
        try:
            connection.request("POST", "/admin/api/logout", body=b"{}",
                               headers={"Content-Type": "application/json", "X-CSRF-Token": csrf,
                                        "Cookie": "rlplayer_admin=" + raw_cookie})
            first = connection.getresponse()
            first.read()
            self.assertEqual(first.status, 200)
            connection.request("GET", "/admin/api/overview",
                               headers={"Cookie": "rlplayer_admin=" + raw_cookie})
            second = connection.getresponse()
            second.read()
            self.assertEqual(second.status, 401)
        finally:
            connection.close()

    def test_overview_reconciles_historical_traffic_gap(self) -> None:
        with self.store.connect() as db:
            db.execute("INSERT OR REPLACE INTO traffic(month,object_bytes,api_bytes) VALUES(?,?,?)",
                       (month_for(), 1000, 0))
        raw_cookie, _ = self.good_login()
        auth = {"Cookie": "rlplayer_admin=" + raw_cookie}
        status, _, overview = self.json_request("GET", "/admin/api/overview", {}, auth)
        self.assertEqual(status, 200)
        traffic = overview["traffic"]
        self.assertEqual(traffic["windows"]["objectBytes"] + traffic["mac"]["objectBytes"],
                         traffic["objectBytes"])
        self.assertGreaterEqual(traffic["objectBytes"], 1000)

    def test_login_rate_limit(self) -> None:
        self.store.config.login_rate = 3
        statuses = [self.post_login("huoyanyan", "bad", self.login_csrf())[0] for _ in range(4)]
        self.assertEqual(statuses[:3], [401] * 3)
        self.assertEqual(statuses[3], 429)


class MacOSHTTPTests(ServiceCase):
    config = {"publish_token_macos": "test-publish-token"}

    @staticmethod
    def object_zip(payload: bytes) -> bytes:
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            info = zipfile.ZipInfo("payload.bin", (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, payload)
        return output.getvalue()

    def upload(self, sha: str, data: bytes, token: str = "test-publish-token") -> tuple[int, dict, bytes]:
        headers = {"Content-Type": "application/zip"}
        if token:
            headers["Authorization"] = "Bearer " + token
        status, response_headers, body = self.request("PUT", "/admin/api/publish/macos/objects/" + sha,
                                                      data, headers)
        return status, {key.lower(): value for key, value in response_headers.items()}, body

    def manifest(self, payload: dict, token: str = "test-publish-token") -> tuple[int, bytes]:
        raw = base64.urlsafe_b64encode(json.dumps(payload, separators=(",", ":")).encode()).decode().rstrip("=")
        envelope = {"alg": "PS256", "kid": "macos-test-key", "payload": raw,
                    "signature": base64.urlsafe_b64encode(b"s" * 384).decode().rstrip("=")}
        body = json.dumps(envelope, separators=(",", ":")).encode()
        headers = {"Content-Type": "application/json"}
        if token:
            headers["Authorization"] = "Bearer " + token
        status, _headers, response = self.request("PUT", "/admin/api/publish/macos/latest", body, headers)
        return status, response

    def payload(self, sha: str, size: int, object_size: int) -> dict:
        now = time.time()
        return {
            "schema": 2, "platform": "macos", "channel": "stable", "metadata_version": 1,
            "version": "3.2.3",
            "published_at": time.strftime("%Y-%m-%dT%H:%M:%S.000000Z", time.gmtime(now - 60)),
            "expires_at": time.strftime("%Y-%m-%dT%H:%M:%S.000000Z", time.gmtime(now + 30 * 86400)),
            "install_mode": "incremental", "reason": "", "notes": "macOS test",
            "files": [{"path": "RLPlayer.app/Contents/MacOS/RLPlayer", "sha256": sha, "size": size,
                       "object": sha + ".zip", "object_sha256": sha, "object_size": object_size, "mode": "0755"}],
            "delete": [],
            "manual": {"cn_url": "https://pan.baidu.com/s/example",
                       "global_url": "https://github.com/okhuoyanyan/RLPlayer-Mac/releases",
                       "extraction_code": "", "sha256": "b" * 64, "size": 123456},
        }

    def test_publish_serve_and_report(self) -> None:
        content = b"macOS binary payload " * 50
        sha = hashlib.sha256(content).hexdigest()
        archive = self.object_zip(content)
        status, _headers, body = self.upload(sha, archive)
        self.assertEqual(status, 200, body)
        self.assertFalse(json.loads(body)["already"])
        status, _headers, body = self.upload(sha, archive)
        self.assertTrue(json.loads(body)["already"])
        status, body = self.manifest(self.payload(sha, len(content), len(archive)))
        self.assertEqual(status, 200, body)
        self.assertEqual(json.loads(body)["version"], "3.2.3")
        status, headers, served = self.request("GET", "/api/v1/update/macos/latest")
        self.assertEqual(status, 200)
        self.assertIn("etag", headers)
        self.assertEqual(json.loads(served)["alg"], "PS256")
        status, headers, downloaded = self.request("GET", "/api/v1/update/macos/objects/" + sha + ".zip")
        self.assertEqual(status, 200)
        self.assertEqual(hashlib.sha256(downloaded).hexdigest(), hashlib.sha256(archive).hexdigest())
        status, headers, part = self.request("GET", "/api/v1/update/macos/objects/" + sha + ".zip",
                                             headers={"Range": "bytes=0-9"})
        self.assertEqual(status, 206)
        self.assertEqual(part, archive[:10])
        self.assertEqual(self.request("GET", "/api/v1/update/windows/latest")[0], 404)
        status, _headers, _body = self.json_request("POST", "/api/v1/update/macos/report",
                                                    {"client": "mac-client-0001", "from": "3.2.2", "to": "3.2.3",
                                                     "ok": True, "ms": 900})
        self.assertEqual(status, 200)
        with self.store.connect() as db:
            row = db.execute("SELECT * FROM events WHERE kind='update_report' ORDER BY id DESC LIMIT 1").fetchone()
        self.assertEqual(row["platform"], "mac")
        self.assertIn("/macos/", row["path"])
        status, _headers, status_body = self.request("GET", "/admin/api/publish/macos/status",
                                                     headers={"Authorization": "Bearer test-publish-token"})
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(status_body)["latest"]["version"], "3.2.3")

    def test_publish_auth_and_validation(self) -> None:
        content = b"payload"
        sha = hashlib.sha256(content).hexdigest()
        archive = self.object_zip(content)
        self.assertEqual(self.upload(sha, archive, token="")[0], 403)
        self.assertEqual(self.upload(sha, archive, token="wrong")[0], 403)
        self.assertEqual(self.upload(sha, archive)[0], 200)
        tampered = self.object_zip(b"different payload")
        self.assertEqual(self.upload(sha, tampered)[0], 422)
        payload = self.payload(sha, len(content), len(archive))
        payload["platform"] = "windows"
        self.assertEqual(self.manifest(payload)[0], 400)
        payload = self.payload(sha, len(content), len(archive))
        payload["files"][0]["sha256"] = "c" * 64
        self.assertEqual(self.manifest(payload)[0], 400)
        payload = self.payload(sha, len(content), len(archive))
        payload["files"][0]["path"] = "../escape"
        self.assertEqual(self.manifest(payload)[0], 400)
        payload = self.payload(sha, len(content), len(archive))
        payload["manual"]["cn_url"] = "http://insecure.example"
        self.assertEqual(self.manifest(payload)[0], 400)
        payload = self.payload("d" * 64, 10, 20)
        self.assertEqual(self.manifest(payload)[0], 409)
        self.assertEqual(self.request("GET", "/admin/api/publish/macos/status")[0], 403)
        self.assertEqual(self.request("GET", "/api/v1/update/macos/latest")[0], 404)

    def test_macos_blacklist_blocks_downloads(self) -> None:
        content = b"macOS payload for blacklist"
        sha = hashlib.sha256(content).hexdigest()
        archive = self.object_zip(content)
        self.assertEqual(self.upload(sha, archive)[0], 200)
        self.assertEqual(self.manifest(self.payload(sha, len(content), len(archive)))[0], 200)
        self.store.add_blacklist("ip", "127.0.0.1", "test")
        status, headers, body = self.request("GET", "/api/v1/update/macos/latest")
        self.assertEqual(status, 429)
        self.assertEqual(json.loads(body)["code"], "quota_exceeded")
        self.assertIn("retry-after", headers)
        self.assertEqual(self.request("GET", "/api/v1/update/macos/objects/" + sha + ".zip")[0], 429)


class RetentionTests(ServiceCase):
    def test_90_day_raw_event_cleanup_preserves_daily_aggregates(self) -> None:
        now = time.time()
        old = now - 91 * 86400
        old_id = self.store.event("api", "127.0.0.1", path="/old", byte_count=123, now=old)
        new_id = self.store.event("api", "127.0.0.1", path="/new", byte_count=456, now=now)
        result = self.store.cleanup(now)
        self.assertEqual(result["deletedEvents"], 1)
        with self.store.connect() as db:
            ids = {row["id"] for row in db.execute("SELECT id FROM events")}
            daily = db.execute("SELECT requests,bytes FROM daily WHERE day=(SELECT day FROM events WHERE id=?)", (new_id,)).fetchone()
            old_daily = db.execute("SELECT requests,bytes FROM daily WHERE requests=1 AND bytes=123").fetchone()
        self.assertNotIn(old_id, ids)
        self.assertIn(new_id, ids)
        self.assertEqual((daily["requests"], daily["bytes"]), (1, 456))
        self.assertIsNotNone(old_daily)


if __name__ == "__main__":
    unittest.main(verbosity=2)
