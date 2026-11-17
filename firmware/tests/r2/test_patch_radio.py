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
# CLI accepts only an audited PIO environment name; never arbitrary paths
# that could bypass the production pinned source/hash verification.
allowed_environments = ("rak4630", "rak4630_orun", "rak4630_kq05_lab")
if len(sys.argv) not in (1, 3):
    raise SystemExit("usage: test_patch_radio.py [output.cpp audited-pio-env]")
selected_env = sys.argv[2] if len(sys.argv) == 3 else "rak4630"
if selected_env not in allowed_environments:
    raise RuntimeError("R2 requires an audited PlatformIO environment")
library = root / "firmware/.pio/libdeps" / selected_env / "SX126x-Arduino"
if json.loads((library / "library.json").read_text())["version"] != "2.0.32":
    raise RuntimeError("R2 requires pinned SX126x-Arduino 2.0.32")
target = library / "src/radio/sx126x/radio.cpp"
original = library / "src/radio/sx126x/radio.cpp.orun-original"
source_file = original if original.exists() else target
if not source_file.exists():
    raise RuntimeError("R2 original radio driver source unavailable")
source = source_file.read_text()
patched = patch.transform(source)  # SHA-256 pinned, rejects any drift.
if original.exists() and target.read_text() != patched:
    raise RuntimeError("R2 installed radio driver differs from verified patch")
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
if len(sys.argv) == 3:
    # Args: output path and the already-verified audited PIO environment.
    # Generate the C++ fixture only after SHA/version/patch checks passed.
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
