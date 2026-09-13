"""Exercise the exact production transform and fail-closed version/source guards."""
import importlib.util
import json
from pathlib import Path
import tempfile
import sys

root = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("patch_radio", root / "firmware/scripts/patch_radio.py")
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)


def rejected(call):
    try:
        call()
    except RuntimeError:
        return
    raise AssertionError("expected fail-closed guard")


rejected(lambda: patch.transform("unknown upstream source"))
library = root / "firmware/.pio/libdeps/rak4630/SX126x-Arduino"
source_file = library / "src/radio/sx126x/radio.cpp.orun-original"
if not source_file.exists():
    source_file = library / "src/radio/sx126x/radio.cpp"
if not source_file.exists():
    raise RuntimeError("Run pio pkg install first: pinned dependency needed for patch guard tests")
source = source_file.read_text()
patched = patch.transform(source)
assert "Guard gate(true)" in patched
assert "IrqFired.exchange(false)" in patched
assert "TimerSetValue(&TxTimeoutTimer, TxTimeout)" not in patched
assert "void RadioOnTxTimeoutIrq(void) {}" in patched
rejected(lambda: patch.transform(source + "\n// drift"))

with tempfile.TemporaryDirectory(prefix="orun-r2-patch-") as directory:
    base = Path(directory)
    lib = base / "library"
    core = base / "core"
    target = lib / "src/radio/sx126x/radio.cpp"
    target.parent.mkdir(parents=True)
    core.mkdir()
    (lib / "library.json").write_text(json.dumps({"version": "2.0.32"}))
    (core / "package.json").write_text(json.dumps({"version": "1.10700.0"}))
    target.write_text(source)
    patch.apply(lib, core)
    assert target.read_text() == patched
    patch.apply(lib, core)  # Idempotent; no double wrapping.
    target.write_text(patched + "// tampered")
    rejected(lambda: patch.apply(lib, core))
    target.write_text(patched)
    (lib / "library.json").write_text(json.dumps({"version": "2.0.33"}))
    rejected(lambda: patch.apply(lib, core))
    (lib / "library.json").write_text(json.dumps({"version": "2.0.32"}))
    (core / "package.json").write_text(json.dumps({"version": "1.10800.0"}))
    rejected(lambda: patch.apply(lib, core))
print("R2.1 patch identity, idempotence and fail-closed guards: PASS")

# Compile the actual patched gate/timeout/quiesce functions into the C++
# interleaving regression, replacing only their hardware-facing operations.
if len(sys.argv) == 2:
    def function(name):
        signature = "void " + name + ("(void)\n{" if name == "RadioBgIrqProcess" else "()\n{")
        start = patched.index(signature)
        opening = patched.index("{", start)
        depth = 1
        end = opening + 1
        while depth:
            depth += (patched[end] == "{") - (patched[end] == "}")
            end += 1
        return patched[start:end] + "\n"

    Path(sys.argv[1]).write_text('#include "driver_patch_model.h"\n' +
        "\n".join(function(name) for name in (
            "RadioBgIrqProcess", "orunRadioTimeoutLocked", "orunRadioQuiesceLocked")))
