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


def parse_result(write_key: bytes, data: bytes) -> Result:
    """The tasma's signed answer; raises BadResult unless the tasma said it."""
    if len(data) != RESULT_SIZE or data[0] != VERSION or data[1] != RESULT_TYPE:
        raise BadResult("shape")
    if not hmac.compare_digest(_tag(write_key, RESULT_DOMAIN, data[:-TAG]), data[-TAG:]):
        raise BadResult("tag")
    (_, _, outcome, _res, device, epoch, cid, inc, rev, interval, window, mode,
     services) = struct.unpack(">BBBBQIIQIIHBB", data[:-TAG])
    if outcome >= len(OUTCOMES):
        raise BadResult("outcome")
    return Result(OUTCOMES[outcome], device, epoch, cid, inc, rev, interval, window, mode, services)
