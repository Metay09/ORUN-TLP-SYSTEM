#!/usr/bin/env python3
"""SF5F step 1 scope: PERIODIC records are stored durably, nothing more.

main.cpp composes ProductRecordStore on the product port and mounts it before
SoftDevice starts. Records are not sealed, sent or released yet; TLP v1
POSITION and HistoryStore stay the live path (docs/milestones/SF5.md).
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]


def code(path: Path) -> str:
    text = path.read_text()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//.*?$", "", text, flags=re.MULTILINE)


def without_ifdef(text: str, macro: str) -> str:
    """Drop `#ifdef macro ... #endif` blocks (nesting-aware), keeping any
    `#else` branch. Used to judge the default image's composition."""
    out, depth, skipping, skip_depth = [], 0, False, 0
    for line in text.splitlines():
        s = line.strip()
        if s.startswith(("#if", "#ifdef", "#ifndef")):
            depth += 1
            if not skipping and s == f"#ifdef {macro}":
                skipping, skip_depth = True, depth
                continue
        elif s.startswith("#else") and skipping and depth == skip_depth:
            skipping = False
            continue
        elif s.startswith("#endif"):
            if depth == skip_depth and skip_depth:
                skipping, skip_depth = False, 0
                depth -= 1
                continue
            depth -= 1
        if not skipping:
            out.append(line)
    return "\n".join(out)


# The SF5L0 bench Gateway image is a separate, non-default composition: its
# product region belongs to the OCQ2 custody store and ProductRecordStore is
# never mounted (GatewayProductStartupGuard refuses otherwise).
main = without_ifdef(code(root / "src" / "main.cpp"), "ORUN_SF5L0_GATEWAY")

# Exactly one owner, on the gate's product port.
assert main.count("orun_tlp::ProductRecordStore product_records(") == 1
assert re.search(r"product_records\(\s*storage_flash_gate\.productPort\(\)",
                 main), "record store must use the gate's product port"
assert main.count("productPort()") == 1, "nothing else may touch the region"

# Mounted at boot before SoftDevice: the synchronous backend refuses after.
mount = main.index("product_records.begin(")
ble = main.index("ble_ready = Bluefruit.begin();")
assert mount < ble, "product region must mount before Bluefruit.begin()"

# Driven only inside the no-TX flash block, like the other stores.
poll = main.index("product_records.poll();")
block = main.rindex("if (!radio_manager.isTransmitting()) {", 0, poll)
assert "geofence_store.poll();" in main[block:poll]

# Not yet: sealing, sending or releasing product records.
for forbidden in ("protectNextPeriodic", "encodeProductSecureFrame",
                  "requestRelease"):
    assert forbidden not in main, f"main.cpp goes beyond SF5F step 1: {forbidden}"

# Only the gate owns the product backend instance.
users = [p.name for p in (root / "src").glob("*.cpp")
         if "NrfProductFlash" in code(p)]
assert sorted(users) == ["nrf_product_flash.cpp"], users
gate_h = code(root / "include" / "flash_mutation_gate.h")
assert "NrfProductFlash sync_product_;" in gate_h
assert re.search(r"kHistory\s*=\s*1,\s*kProduct\s*=\s*2,\s*kConfig\s*=\s*3",
                 gate_h), "Product must be admitted after History, before Config"

print("SF5F step 1 product store scope contract: PASS")
