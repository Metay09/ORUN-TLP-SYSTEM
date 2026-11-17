#!/usr/bin/env python3
"""Tests read-only ELF footprint accounting without requiring a toolchain."""
import importlib.util
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "flash_audit", ROOT / "tools/ci/orun_firmware_flash_audit.py"
)
m = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(m)

SIZE = """sample.elf:
section size addr
.text 459600 155648
.ARM.exidx 8 615248
.data 3684 536895488
.bss 47228 536899172
.heap 184608 536946400
Total 702900
"""
NMS = """00473912 00001696 T BLESecurity::_eventHandler(ble_evt_t*)
00293048 00001704 T setup
00473912 00001696 W BLESecurity alias
536895824 00001840 d (anonymous namespace)::radio_manager
00205236 00003220 T orun_tlp::bridge_frame::formatPositionLine(...)
"""
with patch.object(m, "run", return_value=SIZE):
    s = m.sections(Path("size"), Path("x.elf"))
assert s[".text"] + s[".ARM.exidx"] + s[".data"] == 463292
assert s[".data"] + s[".bss"] == 50912
assert s[".heap"] == 184608  # neither flash nor used static RAM
with patch.object(m, "run", return_value=NMS):
    syms = m.symbols(Path("nm"), Path("x.elf"))
assert len(syms) == 3, syms  # same-address alias dedup; RAM 'd' omitted
assert [x[1] for x in syms] == [1696, 1704, 3220], syms
assert m.bucket("/tmp/project/firmware/src/main.cpp:155") == "ORUN/main.cpp"
assert m.bucket("/tmp/.pio/libdeps/rak4630_orun/SX126x-Arduino/src/radio/radio.cpp:155") == "LoRa/SX126x library"
assert m.bucket("??:0") == "Unattributed (no debug path)"
assert m.bucket(":0") == "Unattributed (no debug path)"  # Actual user's ELF failure.
assert m.bucket(":0", "SFE_UBLOX_GNSS::processUBXpacket(ubxPacket*)") == "GNSS/SparkFun [symbol]"
assert m.bucket(":0", "tud_task_ext") == "USB/TinyUSB [symbol]"
assert m.bucket(":0", "lfs_dir_commit") == "LittleFS/InternalFS [symbol]"
assert m.bucket(":0", "loop") == "ORUN/main runtime [symbol]"
assert m.bucket(":0", "orun_tlp::HistoryStore::poll()") == "ORUN/legacy history [symbol]"
assert m.bucket("??:?", "orun_tlp::ObservationStore::poll()") == "ORUN/product records [symbol]"
assert m.bucket(":0", "orun_tlp::TrackerAcklessLiveTx::update()") == "ORUN/KQ05 [symbol]"
assert m.bucket(":0", "orun_tlp::Kq05BackendReceiptVerifier::verify()") == "ORUN/KQ05 [symbol]"
assert m.bucket(":0", "_GLOBAL__sub_I__Z21decideBootStorageRolev") == "C++ global initialization [symbol]"
assert m.bucket("/tmp/project/firmware/src/main.cpp:0", "loop") == "ORUN/main.cpp"
assert m.bucket(":0", "weird_upstream_function()") == "Unknown linked symbol (source unavailable)"

with patch.object(m, "run", return_value="x.cpp:1\ny.cpp:2\nz.cpp:3\n"):
    assert len(m.origins(Path("addr2line"), Path("x.elf"), syms)) == 3
print("KQ05 read-only ELF size/symbol/source auditor: PASS (synthetic fixtures)")
