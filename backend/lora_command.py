"""6: tasmaya LoRa'dan ayar komutu (sahip kararı 2026-11-11).

Komutu sunucu, telefonların Bluetooth'ta kullandığı tasma başı yazma
anahtarıyla (orun_credentials.ble_write_key) imzalar; gateway yalnız taşır.
Komut tasmanın kayıtlı ayar durumuna (state token) bağlıdır: uygulanınca
durum ilerler, aynı komut tekrar gelirse hiçbir şey değişmez (STALE).
Baytlar firmware/include/lora_command.h ile aynı; ortak vektör iki tarafta.
"""
from __future__ import annotations

from dataclasses import dataclass
import hmac
import hashlib
import struct

VERSION = 0x02
COMMAND_TYPE = 0x07
RESULT_TYPE = 0x08
RESOURCE_SETTINGS = 1
RESOURCE_AREAS = 2
AREA_CHUNK = 202          # 255 - 37 header - 16 tag
MAX_AREAS, MAX_VERTICES = 8, 64
COMMAND_DOMAIN = b"ORUN-LORA-CMD-v1"
RESULT_DOMAIN = b"ORUN-LORA-RES-v1"
TAG = 16
MAX_ENTRIES = 8
RESULT_SIZE = 56

# Tek ayar tablosu (firmware settings_table.cpp): numara -> ad.
SETTINGS = {1: "INTERVAL", 2: "SERVICES", 3: "BLE_WINDOW"}
OUTCOMES = ["APPLIED", "UNCHANGED", "INVALID", "ACCESS_DENIED", "MAINTENANCE",
            "UNAVAILABLE", "OUTCOME_UNKNOWN", "STALE"]


class BadResult(ValueError):
    pass


def _tag(key: bytes, domain: bytes, body: bytes) -> bytes:
    return hmac.new(key, domain + body, hashlib.sha256).digest()[:TAG]


def build_command(write_key: bytes, device: int, epoch: int, command_id: int,
                  incarnation: int, revision: int, entries: list[tuple[int, int]]) -> bytes:
    """Signed settings command for one tasma; entries = [(setting id, value)]."""
    if not 1 <= len(entries) <= MAX_ENTRIES:
        raise ValueError("entries")
    ids = [i for i, _ in entries]
    if len(set(ids)) != len(ids) or any(i not in SETTINGS for i in ids):
        raise ValueError("setting")
    body = struct.pack(">BBBBQIIQIB", VERSION, COMMAND_TYPE, 0, RESOURCE_SETTINGS, device, epoch,
                       command_id, incarnation, revision, len(entries))
    for sid, value in entries:
        body += struct.pack(">BI", sid, value)
    return body + _tag(write_key, COMMAND_DOMAIN, body)


def area_set(polys: list[list[tuple[int, int]]]) -> bytes:
    """S = n_areas, each area's vertex count, then every (lat_e7, lon_e7)."""
    if len(polys) > MAX_AREAS or sum(len(p) for p in polys) > MAX_VERTICES or any(not p for p in polys):
        raise ValueError("areas")
    out = bytes([len(polys)]) + bytes(len(p) for p in polys)
    for p in polys:
        for lat, lon in p:
            out += struct.pack(">ii", lat, lon)
    return out


def build_area_command(write_key: bytes, device: int, epoch: int, command_id: int,
                       incarnation: int, revision: int, polys: list[list[tuple[int, int]]]) -> list[bytes]:
    """Signed area list (1..3 frames); polys = [] removes every area."""
    s = area_set(polys)
    vertices = sum(len(p) for p in polys)
    parts = (len(s) + AREA_CHUNK - 1) // AREA_CHUNK
    frames = []
    for i in range(parts):
        body = struct.pack(">BBBBQIIQIBBBH", VERSION, COMMAND_TYPE, 0, RESOURCE_AREAS, device, epoch,
                           command_id, incarnation, revision, i, parts, len(polys), vertices)
        body += s[i * AREA_CHUNK:(i + 1) * AREA_CHUNK]
        frames.append(body + _tag(write_key, COMMAND_DOMAIN, body))
    return frames


@dataclass(frozen=True)
class Result:
    outcome: str
    device: int
    epoch: int
    command_id: int
    incarnation: int
    revision: int
    interval_s: int
    ble_window_s: int
    service_mode: int
    requested_services: int
    resource: int = RESOURCE_SETTINGS
    area_state: int = 0      # areas: 0 clear, 1 configured, 2 unavailable
    areas: int = 0
    vertices: int = 0


def parse_result(write_key: bytes, data: bytes) -> Result:
    """The tasma's signed answer; raises BadResult unless the tasma said it."""
    if len(data) != RESULT_SIZE or data[0] != VERSION or data[1] != RESULT_TYPE:
        raise BadResult("shape")
    if not hmac.compare_digest(_tag(write_key, RESULT_DOMAIN, data[:-TAG]), data[-TAG:]):
        raise BadResult("tag")
    (_, _, outcome, res, device, epoch, cid, inc, rev) = struct.unpack(">BBBBQIIQI", data[:32])
    if outcome >= len(OUTCOMES) or res not in (RESOURCE_SETTINGS, RESOURCE_AREAS):
        raise BadResult("outcome")
    if res == RESOURCE_AREAS:
        state, areas, vertices = struct.unpack(">BBH", data[32:36])
        return Result(OUTCOMES[outcome], device, epoch, cid, inc, rev, 0, 0, 0, 0,
                      RESOURCE_AREAS, state, areas, vertices)
    interval, window, mode, services = struct.unpack(">IHBB", data[32:40])
    return Result(OUTCOMES[outcome], device, epoch, cid, inc, rev, interval, window, mode, services)
