#!/usr/bin/env python3
"""KQ05 28KiB boot diagnostic: conservative source contract (PC only).

This is a source/feature-guard test, NOT proof of a target build,
physical Flash takeover, device role authority or on-air custody ACK.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
MAIN = (ROOT / "firmware/src/main.cpp").read_text(encoding="utf-8")
CENSUS = (ROOT / "firmware/src/kq05_supplemental_28k_census.cpp").read_text(
    encoding="utf-8"
)
FLAG = "ORUN_KQ05_SUPPLEMENTAL_28K_PREFLIGHT"
PLATFORMIO = (ROOT / "firmware/platformio.ini").read_text(encoding="utf-8")


def require(condition: bool, explanation: str) -> None:
    if not condition:
        raise AssertionError(explanation)


def check() -> None:
    require(FLAG not in PLATFORMIO,
            "read-only preflight must NOT be enabled in a build environment")
    require('#ifdef ' + FLAG + '\n#include "kq05_supplemental_28k_census.h"'
            in MAIN, "diagnostic header not gated")
    require(('#if defined(' + FLAG + ')') in MAIN and
            '!defined(ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE)' in MAIN,
            "must refuse enablement outside explicit KQ05 test image")

    # Extract only the preflight-specific block after the role decision.
    start = MAIN.index('#ifdef ' + FLAG + '\n  // Explicit opt-in diagnostic')
    stop = MAIN.index('#ifdef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE', start)
    preflight = MAIN[start:stop]
    require('decideBootStorageRole();' in MAIN[max(0, start-180):start],
            "the storage role must be decided BEFORE 28KiB preflight")
    require('storage_flash_gate.begin()' in preflight,
            "physical read-only backend must begin before census")
    require('kq05_supplemental::inspectReadOnly(' in preflight,
            "must inspect complete old History bytes")
    require('device_identity.legacyUint64()' in preflight,
            "must supply local hardware-derived identity")
    require('kBlockedRole' in preflight and 'kReadFailed' in preflight,
            "unknown/failed role must fail closed")
    require('WRITER=DISABLED; NO_MIGRATION' in preflight,
            "do not report supplemental as mounted")
    require(not re.search(r'\.program\(|\.erasePage\(|\bhistory\.begin\(', preflight),
            "read-only preflight must never mutate/mount history")

    branch_start = MAIN.index(
        '} else if (storage_role == orun_tlp::StorageRole::kGateway) {',
        stop
    )
    branch_end = MAIN.index(
        '} else {\n    Serial.println(F("KQ05 STORAGE role unassigned/conflict;',
        branch_start,
    )
    gateway_block = MAIN[branch_start:branch_end]
    require('#ifdef ' + FLAG in gateway_block,
            "Gateway must have opt-in legacy exclusion")
    require('Gateway HistoryStore NOT MOUNTED' in gateway_block,
            "Gateway must report legacy disabled on cutover probe")
    first_else = gateway_block.index('#else')
    require('history.begin(' not in gateway_block[:first_else],
            "Gateway must not mount HistoryStore in opt-in branch")
    require('history.begin(' in gateway_block[first_else:],
            "normal KQ05 Gateway must retain existing legacy behavior")
    require('#endif' in gateway_block[first_else:],
            "Gateway dual-mode branch must close correctly")

    require(
        '#ifndef ' + FLAG +
        '\n    if (!kq05ProductOnlyTracker()) history.poll();' in MAIN,
        "HistoryStore polling must be forbidden in opt-in read-only image"
    )
    require('history_port.read(' in CENSUS,
            "census must read real old History bytes")
    require('.erasePage(' not in CENSUS and '.program(' not in CENSUS,
            "census must never write or erase")
    require('for (uint32_t p = 0U; p < kPages; ++p)' in CENSUS,
            "census must inspect all seven pages")
    print("KQ05 28KiB opt-in boot role/census/legacy-exclusion source: PASS (PC ONLY)")
    print("NO PRODUCT CUTOVER, NO FLASH ERASE, NO DEVICE UPLOAD, NO ACK")


if __name__ == "__main__":
    check()
