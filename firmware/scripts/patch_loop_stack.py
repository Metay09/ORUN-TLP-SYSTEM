"""Pinned transform: larger Arduino loop-task stack for Adafruit nRF52 1.7.0.

The core runs setup() and loop() on one FreeRTOS task whose stack it fixes
at 256*4 words (4 KiB) in cores/nRF5/main.cpp, with no override hook.
SparkFun u-blox GNSS 2.2.29 SFE_UBLOX_GNSS::checkCallbacks() alone has a
3,096-byte frame, and a RAK4631 tracker measured zero free loop-task stack
(HEALTH stack_free=0) while it was being reset by the watchdog. This raises
the loop task to 256*8 words (8 KiB) of the ~210 KiB heap.

Like the R4 Wire patch, the framework source is patched only for the
PlatformIO process and restored at normal exit; an unrecognized source fails
closed. Host tests exercise the pure transform.
"""
import atexit
import hashlib
import json
from pathlib import Path

ORIGINAL_GIT_BLOB_SHA = "7e3e95f82be564469ef6489e84419308a2727c60"
MARKER = "// ORUN loop-task stack 8 KiB\n"
ORIGINAL_DEFINE = "#define LOOP_STACK_SZ       (256*4)\n"
PATCHED_DEFINE = "#define LOOP_STACK_SZ       (256*8)\n"


def git_blob_sha(source):
    data = source.encode()
    header = f"blob {len(data)}\0".encode()
    return hashlib.sha1(header + data).hexdigest()


def transform(source):
    if git_blob_sha(source) != ORIGINAL_GIT_BLOB_SHA:
        raise RuntimeError(
            "loop stack: unrecognized cores/nRF5/main.cpp; dependency re-audit required")
    if source.count(ORIGINAL_DEFINE) != 1:
        raise RuntimeError("loop stack: LOOP_STACK_SZ definition changed; re-audit required")
    return MARKER + source.replace(ORIGINAL_DEFINE, PATCHED_DEFINE, 1)


def apply(core):
    if json.loads((core / "package.json").read_text())["version"] != "1.10700.0":
        raise RuntimeError("loop stack patch requires Adafruit nRF52 1.7.0 re-audit")

    target = core / "cores/nRF5/main.cpp"
    backup = core / "cores/nRF5/main.cpp.orun-original"
    source = target.read_text()

    if source.startswith(MARKER):
        if not backup.exists():
            raise RuntimeError("loop stack: patched main.cpp found without original backup")
        original = backup.read_text()
        if source != transform(original):
            raise RuntimeError("loop stack: patched main.cpp altered; re-audit required")
    else:
        original = source
        patched = transform(original)  # Validate exact upstream before writing.
        if backup.exists() and backup.read_text() != original:
            raise RuntimeError("loop stack: backup does not match audited upstream")
        if not backup.exists():
            backup.write_text(original)
        target.write_text(patched)

    def restore():
        try:
            if target.exists() and target.read_text().startswith(MARKER):
                target.write_text(original)
        except Exception:
            # The build result stays authoritative; the next run validates
            # marker + backup and fails closed if restoration was interrupted.
            pass

    atexit.register(restore)
    print("LOOP STACK: verified/applied 8 KiB Adafruit nRF52 1.7.0 loop-task stack")


if "Import" in globals():
    Import("env")
    apply(Path(env.PioPlatform().get_package_dir("framework-arduinoadafruitnrf52")))
