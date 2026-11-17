#!/usr/bin/env python3
"""Read-only linked ELF footprint audit for normal ORUN and KQ05 LAB.

Uses existing PlatformIO artifacts only. No build, upload, Flash access,
systemd action, credentials, or production DB writes.
Symbol aggregation is an estimate, NOT proof that a feature can be removed.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
ENVS = ("rak4630_orun", "rak4630_kq05_lab")
NM_RE = re.compile(r"^\s*(\d+)\s+(\d+)\s+([A-Za-z])\s+(.+)$")
SECTION_RE = re.compile(r"^\s*(\S+)\s+(\d+)\s+\d+\s*$")
FLASH_SYMBOL_TYPES = set("TtRrWwVv")


def tools(prefix: Path) -> dict[str, Path]:
    result = {}
    for tool in ("size", "nm", "addr2line"):
        path = prefix / ("arm-none-eabi-" + tool)
        if not path.is_file():
            found = shutil.which("arm-none-eabi-" + tool)
            if found is None:
                raise RuntimeError(f"ARM tool not installed: {path}")
            path = Path(found)
        result[tool] = path
    return result


def run(command: list[str]) -> str:
    cp = subprocess.run(command, capture_output=True, text=True, check=False)
    if cp.returncode != 0:
        raise RuntimeError(f"Command failed ({cp.returncode}): {command[0]}: {cp.stderr[-500:]}")
    return cp.stdout


def sections(size_bin: Path, elf: Path) -> dict[str, int]:
    out = {}
    for line in run([str(size_bin), "-A", str(elf)]).splitlines():
        m = SECTION_RE.match(line)
        if m and m.group(1).startswith("."):
            out[m.group(1)] = int(m.group(2))
    if ".text" not in out or ".data" not in out or ".bss" not in out:
        raise RuntimeError(f"ELF sections missing: {elf}")
    return out


def symbols(nm_bin: Path, elf: Path) -> list[tuple[int, int, str, str]]:
    out = []
    for line in run([str(nm_bin), "-S", "-C", "--defined-only", "--radix=d", str(elf)]).splitlines():
        m = NM_RE.match(line)
        if not m or m.group(3) not in FLASH_SYMBOL_TYPES:
            continue
        address, size = int(m.group(1)), int(m.group(2))
        if size > 0:
            out.append((address, size, m.group(3), m.group(4)))
    # Same-address aliases are not two different occupied byte ranges.
    distinct: dict[tuple[int, int], tuple[int, int, str, str]] = {}
    for s in out:
        distinct.setdefault((s[0], s[1]), s)
    return list(distinct.values())


def origins(addr_bin: Path, elf: Path, sym: list[tuple[int, int, str, str]]) -> list[str]:
    found = []
    for start in range(0, len(sym), 400):
        batch = sym[start:start + 400]
        args = [str(addr_bin), "-e", str(elf)] + [hex(s[0]) for s in batch]
        entries = run(args).splitlines()
        if len(entries) != len(batch):
            raise RuntimeError(f"Unexpected addr2line results: {elf}")
        found += entries
    return found


def symbol_bucket(symbol: str) -> str:
    """Fallback when ELF has no DWARF line info. This is NOT file attribution."""
    x = symbol.lower()
    if symbol in ("setup", "loop"):
        return "ORUN/main runtime [symbol]"
    if "handlerolecommand" in x:
        return "ORUN/USB and role commands [symbol]"
    if x.startswith("sfe_ublox_gnss::") or x.startswith("ubx"):
        return "GNSS/SparkFun [symbol]"
    if x.startswith(("tud_", "tuh_", "tusb_", "usbd_", "dcd_", "hcd_",
                     "mscd_", "videod_", "audiod_", "cdc_", "hid_", "msc_")):
        return "USB/TinyUSB [symbol]"
    if x.startswith(("lfs_", "littlefs")):
        return "LittleFS/InternalFS [symbol]"
    if (x.startswith(("ble", "bluefruit", "adafruit_ble")) or
            "blegatt" in x or "ble_gap" in x):
        return "BLE/Bluefruit [symbol]"
    if x.startswith(("pka", "cc310", "mbedtls_", "nrf_crypto", "nrf_cc310")):
        return "Crypto libraries [symbol]"
    if x.startswith(("xqueue", "xsemaphore", "xtask", "vtask", "vport", "pvport",
                     "xeventgroup", "xtimer", "vlist")):
        return "RTOS/FreeRTOS [symbol]"
    if x.startswith(("sx126", "radioevents")):
        return "LoRa/SX126x [symbol]"
    if x.startswith("_global__sub_i_"):
        return "C++ global initialization [symbol]"
    if x.startswith("orun_tlp::"):
        if any(s in x for s in ("trackeracklesslivetx", "kq05", "batchreceipt")):
            return "ORUN/KQ05 [symbol]"
        if any(s in x for s in ("trackerproducttxpilot", "sf5l0", "drainpacer")):
            return "ORUN/legacy pilot & bench [symbol]"
        if any(s in x for s in ("historystore", "historydelivery",
                                 "historyreceipt", "historysecure", "tlp::history")):
            return "ORUN/legacy history [symbol]"
        if any(s in x for s in ("observationstore", "productrecord",
                                 "productperiod", "observation_store",
                                 "productsecure", "productcustody")):
            return "ORUN/product records [symbol]"
        if "geofence" in x:
            return "ORUN/geofence [symbol]"
        if any(s in x for s in ("configstore", "config_format", "configmutation",
                                 "configincarnation", "runtimeconfig")):
            return "ORUN/config [symbol]"
        if any(s in x for s in ("securitystore", "security_v3",
                                 "security_format", "custodyack", "grant",
                                 "signed", "ed25519")):
            return "ORUN/security & authorization [symbol]"
        if any(s in x for s in ("ble", "applicationtransport", "applicationhandoff")):
            return "ORUN/BLE app [symbol]"
        if any(s in x for s in ("gnss", "positionflow", "locationowner")):
            return "ORUN/location flow [symbol]"
        if any(s in x for s in ("radiomanager", "networkservice", "lora",
                                 "relayforward", "bridge_frame", "tlp_position")):
            return "ORUN/LoRa & bridge [symbol]"
        if any(s in x for s in ("battery", "power", "accelerometer",
                                 "activity", "sensor")):
            return "ORUN/power & sensors [symbol]"
        return "ORUN/other [symbol]"
    return "Unknown linked symbol (source unavailable)"


def bucket(origin: str, symbol: str = "") -> str:
    """Prefer actual addr2line paths. If absent, label symbol-name estimates."""
    path = origin.replace("\\", "/").lower().strip()
    if re.search(r":(?:\d+|\?)(?: \(discriminator \d+\))?$", path):
        path = path.rsplit(":", 1)[0]
    # Stripped DWARF often yields ':0', '??:0', '??:?' or an empty string.
    # Never label all of those 'Other/external ()' as though they were a file.
    if path in ("", "?", "??", "<unknown>") or path.startswith("??"):
        return symbol_bucket(symbol) if symbol else "Unattributed (no debug path)"
    if "firmware/src/" in path or path.startswith("src/"):
        tail = path.split("firmware/src/")[-1]
        return "ORUN/" + Path(tail).name
    if "firmware/include/" in path:
        return "ORUN/include/" + Path(path).name
    if "sx126x" in path or "lorawan" in path:
        return "LoRa/SX126x library"
    if "sparkfun" in path or "sfe_ublox" in path:
        return "GNSS/SparkFun library"
    if "tinyusb" in path or "tusb" in path or "usbd" in path:
        return "USB/TinyUSB library"
    if "bluefruit" in path or "blegap" in path or "bled" in path:
        return "BLE/Bluefruit library"
    if "littlefs" in path or "/lfs" in path or "internalfilesystem" in path:
        return "LittleFS/InternalFS library"
    if "nrfcrypto" in path or "nrf_cc310" in path or "oberon" in path:
        return "Crypto libraries"
    if "frameworkarduino" in path or "cores/nrf5" in path:
        return "Arduino/framework"
    return "Other/external (" + (Path(path.split(":")[0]).name or "unknown") + ")"


def analyze(elf: Path, toolbins: dict[str, Path], top: int) -> dict:
    sec = sections(toolbins["size"], elf)
    syms = symbols(toolbins["nm"], elf)
    refs = origins(toolbins["addr2line"], elf, syms)
    groups = defaultdict(int)
    names = defaultdict(int)
    for (_, size, _, name), source in zip(syms, refs):
        groups[bucket(source, name)] += size
        names[name] += size
    flash = sec[".text"] + sec.get(".ARM.exidx", 0) + sec[".data"]
    print("\n===", elf.parent.name, "===")
    print(f"Flash ELF (.text+.ARM.exidx+.data): {flash:,} B | .text {sec['.text']:,} B | .data {sec['.data']:,} B")
    print(f"RAM static (.data+.bss): {sec['.data'] + sec['.bss']:,} B | .heap is a reservation, NOT flash")
    accounted = sum(groups.values())
    print(f"Named symbols (estimation, not directly equivalent to Flash): {accounted:,} B / .text {sec['.text']:,} B")
    missing = sum(ref.strip().lower() in ("", ":0", "??:0", "??:?", "??", "?")
                  for ref in refs)
    print(f"Unknown line locations: {missing:,} / {len(refs):,} symbols")
    if missing:
        print("WARNING: [symbol] buckets are name-based estimates, not source/object attribution.")
    print("Largest linked symbol categories (not removable-size estimates):")
    for name, size in sorted(groups.items(), key=lambda x: -x[1])[:max(0, top)]:
        print(f"  {size:>8,} B  {name}")
    print("Largest named symbols:")
    for name, size in sorted(names.items(), key=lambda x: -x[1])[:min(max(0, top), 25)]:
        print(f"  {size:>8,} B  {name[:112]}")
    return {"flash": flash, "text": sec[".text"], "ram_static": sec[".data"] + sec[".bss"], "groups": groups}


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--top", type=int, default=18)
    p.add_argument("--firmware-dir", type=Path, default=ROOT / "firmware")
    p.add_argument("--tool-dir", type=Path,
                   default=Path.home() / ".platformio/packages/toolchain-gccarmnoneeabi/bin")
    args = p.parse_args()
    try:
        bins = tools(args.tool_dir)
        data = {}
        for env in ENVS:
            elf = args.firmware_dir / ".pio" / "build" / env / "firmware.elf"
            if not elf.is_file():
                raise RuntimeError(f"Missing ELF: {elf}; no auto-build performed")
            data[env] = analyze(elf, bins, args.top)
        normal, lab = (data[x] for x in ENVS)
        print("\n=== KQ05 LAB minus NORMAL (linked image, NOT isolated KQ05 code size) ===")
        print(f"Flash difference: {lab['flash']-normal['flash']:+,} B")
        print(f"Static RAM difference: {lab['ram_static']-normal['ram_static']:+,} B")
        print("Linked symbol categories with largest estimated changes (NOT removable bytes):")
        changes = [(key, lab["groups"].get(key, 0) - normal["groups"].get(key, 0))
                   for key in set(lab["groups"]) | set(normal["groups"])]
        for key, delta in sorted(changes, key=lambda x: -abs(x[1]))[:max(0, args.top)]:
            if delta:
                print(f"  {delta:+8,} B  {key}")
        print("\nREAD-ONLY: no build, no firmware upload, no Flash, no DB changes")
        print("NOTE: symbol totals omit unnamed/padding/overlaps and can misattribute inlined functions.")
        return 0
    except (RuntimeError, OSError) as exc:
        print(f"AUDIT STOP: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
