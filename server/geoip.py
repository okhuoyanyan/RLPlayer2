# -*- coding: utf-8 -*-
"""离线 IP 归属地查询（ip2region v4 xdb）。

数据与读取器来自 ip2region（Apache-2.0），见 server/third_party/ip2region/。
只做 IPv4；内网/保留地址返回 reserved=True。查询失败一律降级为空字符串，
绝不让归属地查询影响主服务可用性。
"""
from __future__ import annotations

import io
import os
import sys
import threading
from typing import Any

_BASE = os.path.dirname(os.path.abspath(__file__))
_THIRD = os.path.join(_BASE, "third_party")
if _THIRD not in sys.path:
    sys.path.insert(0, _THIRD)

_DEFAULT_DB = os.path.join(_BASE, "data", "ip2region_v4.xdb")

_EMPTY = {"country": "", "province": "", "city": "", "isp": "", "code": "", "reserved": False}


class GeoLookup:
    def __init__(self, db_path: str = "", cache_limit: int = 8192) -> None:
        self.path = db_path or _DEFAULT_DB
        self.cache_limit = cache_limit
        self.error = ""
        self._lock = threading.Lock()
        self._searcher: Any = None
        self._cache: dict[str, dict[str, Any]] = {}
        self._load()

    def _load(self) -> None:
        try:
            import ip2region.util as util
            from ip2region.searcher import Searcher
        except Exception as error:  # pragma: no cover - 缺文件时降级
            self.error = "ip2region unavailable: %s" % error
            return
        try:
            if not os.path.isfile(self.path):
                raise FileNotFoundError(self.path)
            with io.open(self.path, "rb") as handle:
                header = util.load_header(handle)
                version = util.version_from_header(header)
                if version is None or getattr(version, "byte_num", 0) != 4:
                    raise ValueError("xdb is not an IPv4 database")
                vector = util.load_vector_index(handle)
            self._searcher = Searcher(version, self.path, vector, None)
        except Exception as error:
            self.error = "geo db load failed: %s" % error
            self._searcher = None

    @property
    def available(self) -> bool:
        return self._searcher is not None

    def close(self) -> None:
        searcher = self._searcher
        self._searcher = None
        if searcher is not None:
            try:
                searcher.close()
            except Exception:
                pass

    def __del__(self) -> None:  # pragma: no cover - 解释器退出时清理
        self.close()

    def lookup(self, ip: str) -> dict[str, Any]:
        if not ip or self._searcher is None:
            return dict(_EMPTY)
        cached = self._cache.get(ip)
        if cached is not None:
            return cached
        result = dict(_EMPTY)
        try:
            raw = self._searcher.search(ip) or ""
        except Exception:
            raw = ""
        if raw:
            parts = raw.split("|")
            while len(parts) < 5:
                parts.append("")
            country, province, city, isp, code = parts[:5]
            def clean(value: str) -> str:
                return "" if value in ("0", "-", "") else value
            result = {
                "country": clean(country),
                "province": clean(province),
                "city": clean(city),
                "isp": clean(isp),
                "code": clean(code),
                "reserved": country.strip().lower() in ("reserved", "内网", "局域网"),
            }
        with self._lock:
            if len(self._cache) >= self.cache_limit:
                self._cache.clear()
            self._cache[ip] = result
        return result
