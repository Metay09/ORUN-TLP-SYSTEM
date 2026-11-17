#!/usr/bin/env python3
"""KQ05 build-time source ownership: no old exact-cache pilot in ACKless image.

Preprocesses only main.cpp's #if control-flow (no board headers/hardware).
The actual PlatformIO link is checked separately by the trim closure gate.
"""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]
MAIN = (ROOT / "firmware/src/main.cpp").read_text()
compiler = shutil.which("g++")
if not compiler:
    raise RuntimeError("C++ preprocessor required for KQ05 source guard")

lines = []
continuation = False
for line in MAIN.splitlines():
    stripped = line.lstrip()
    if continuation:
        lines.append(line)
        continuation = line.rstrip().endswith("\\")
        continue
    if stripped.startswith(("#if ", "#if\t", "#ifdef ", "#ifndef ",
                            "#elif ", "#else", "#endif", "#define ", "#undef ")):
        lines.append(line)
        continuation = line.rstrip().endswith("\\")
    elif "TrackerProductTxPilot tracker_product_tx" in line:
        lines.append("OLD_PILOT_INSTANCE")
    elif "tracker_product_tx." in line:
        lines.append("OLD_PILOT_CALL")
    elif "kq05_live_tx." in line:
        lines.append("KQ05_TX_CALL")

filtered = "\n".join(lines) + "\n"
shared = ["-DORUN_SF5K3V_TRACKER_TX_PILOT",
          "-DORUN_SF5L0_TRACKER",
          "-DORUN_SF5L0_GATEWAY",
          "-DORUN_STEP3B_GATEWAY_ENROLLMENT"]


def preprocessed(additional: list[str]) -> str:
    p = subprocess.run([compiler, "-E", "-P", "-x", "c++", *shared,
                        *additional, "-"],
                       input=filtered, text=True, capture_output=True)
    if p.returncode:
        raise RuntimeError("preprocessor guard failed:\n" + p.stderr[-2500:])
    return p.stdout


normal = preprocessed([])
kq05 = preprocessed(["-DORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE"])
assert normal.count("OLD_PILOT_INSTANCE") == 1, "normal pilot removed!"
assert normal.count("OLD_PILOT_CALL") >= 8, "normal pilot behavior modified!"
assert "KQ05_TX_CALL" not in normal
assert "OLD_PILOT_INSTANCE" not in kq05, "KQ05 links old pilot object!"
assert "OLD_PILOT_CALL" not in kq05, "KQ05 still calls old pilot!"
assert kq05.count("KQ05_TX_CALL") >= 3, "KQ05 sender missing"
assert "KQ05 DRAIN ON refused: legacy ACK pilot not in image" in MAIN
assert "KQ05 DRAIN LIVE refused: experimental LIVE is automatic" in MAIN
assert "KQ05 DRAIN OFF refused: use APP SERVICES NONE" in MAIN
print("KQ05 old pilot excluded / normal bench pilot preserved: PASS (preprocessor source only)")
