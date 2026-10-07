#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def strip_cpp_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//.*", "", text)


store_h = strip_cpp_comments(read("firmware/include/custody_store.h"))
format_h = strip_cpp_comments(read("firmware/include/custody_store_format.h"))
storage = strip_cpp_comments(read("firmware/include/storage_config.h"))
flash_backend = strip_cpp_comments(read("firmware/include/flash_backend.h"))
main = strip_cpp_comments(read("firmware/src/main.cpp"))

# SF4B is a portable persistence foundation only. It must not allocate or wire
# a production gateway partition/runtime while SF4C security/wire and the
# profile-specific flash ownership review are still pending.
assert "CustodyStore" not in main
assert "custody_store.h" not in main
assert "custody" not in storage.lower()
assert "NrfCustodyFlash" not in flash_backend

# Keep the store below RF/security/Edge/backend owners. The only protocol fact
# format code may bind is the already-frozen current HISTORY_SECURE observation
# object size.
for forbidden_include in (
    '"radio_manager.h"',
    '"network_service.h"',
    '"security_store.h"',
    '"history_store.h"',
    '"ble_application_transport.h"',
):
    assert forbidden_include not in store_h
    assert forbidden_include not in format_h

assert '"storage_config.h"' not in store_h
assert '"storage_config.h"' not in format_h
assert '"flash_backend.h"' in store_h
assert '"tlp_v2_history_secure.h"' in format_h
assert "kHistoryObservationPacketSize" in format_h
assert "kObjectSize == 73U" in format_h
assert "kVersion = 2U" in format_h
assert "kRecordSize = 92U" in format_h
assert "kRecordsPerPage == 43U" in format_h
assert "kIntentSlotsPerPage = 3U" in format_h
assert "kIntentCompleteOffset = 20U" in format_h
assert "kRecordHandoff0Offset = 84U" in format_h
assert "kRecordHandoff1Offset = 88U" in format_h

print("SF4B source ownership/activation contract: PASS")
