"""M7P4 pinned transform relocating the Adafruit nRF52 InternalFS (used for
BLE bond storage via Bluefruit's bond_init() -> InternalFS.begin()) out of
the ORUN history journal region (0xED000..0xF4000) and into the M7P1-decided,
M7P2-reserved BLE bond partition (0xEB000..0xED000, 2 pages / 0x2000 bytes).

The framework source is patched only for the PlatformIO process and restored
at normal process exit, exactly like the existing R4 Wire patch
(patch_wire.py) and R2.1 SX126x patch (patch_radio.py). A backup also
permits fail-closed recovery after an interrupted build. Host tests exercise
the pure transform; this script only applies it to the actual installed
package and restores it.

This does not enable BLE, patch Bluefruit/bonding.cpp, or invent a new bond
file format -- it only relocates where the stock InternalFS/LittleFS backend
looks for its partition, so a future slice that does enable BLE cannot
collide with the history journal.
"""
import atexit
import hashlib
import json
from pathlib import Path

ORIGINAL_GIT_BLOB_SHA = "923aede3cd736ac4a7caf75fed982aeb2c726136"
MARKER = "// ORUN M7P4 relocated BLE bond InternalFS partition\n"

# Single source of truth for the target/backup paths, relative to the
# framework package root ("InternalFileSytem" is the vendor's own spelling,
# not a typo introduced here). check_storage_layout.py imports these instead
# of hardcoding its own copies, so the two scripts cannot silently drift
# apart if this path is ever revised.
RELATIVE_TARGET_PATH = "libraries/InternalFileSytem/src/InternalFileSystem.cpp"
RELATIVE_BACKUP_PATH = RELATIVE_TARGET_PATH + ".orun-original"


def target_and_backup_paths(core):
    return core / RELATIVE_TARGET_PATH, core / RELATIVE_BACKUP_PATH

OLD_ADDR_LINE = "#define LFS_FLASH_ADDR        0xED000\n"
NEW_ADDR_LINE = ("#define LFS_FLASH_ADDR        0x0EB000  "
                 "// ORUN M7P4: relocated BLE bond partition\n")
OLD_SIZE_LINE = "#define LFS_FLASH_TOTAL_SIZE  (7*FLASH_NRF52_PAGE_SIZE)\n"
NEW_SIZE_LINE = ("#define LFS_FLASH_TOTAL_SIZE  (2*FLASH_NRF52_PAGE_SIZE)  "
                 "// ORUN M7P4: 2 pages, 0x2000\n")


def git_blob_sha(source):
    data = source.encode()
    header = f"blob {len(data)}\0".encode()
    return hashlib.sha1(header + data).hexdigest()


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise RuntimeError(
            "M7P4 InternalFileSystem.cpp fragment changed; dependency re-audit required")
    return source.replace(old, new, 1)


def relocation_ok(source_text):
    """Pure check (no filesystem access): does this InternalFileSystem.cpp
    source text show the exact M7P4 relocation (marker present, both
    relocated definitions present, *both* stock definitions absent) rather
    than a stock, partially-patched, or otherwise drifted definition? This
    is the single source of truth check_storage_layout.py's post-link
    ownership guard reuses (imported, not duplicated) to distinguish a
    correctly M7P4-relocated InternalFS from a stock or unrecognized one
    that would overlap history.

    Both the new address/size definitions must be present AND both stock
    definitions must be absent: checking only the address (or only the
    size) would accept a file that still carries the other stock `#define`
    alongside the new one -- e.g. a corrupted or hand-edited file with the
    relocated address but the original 7-page size still present, which
    would not actually be the audited M7P4 partition geometry.
    """
    return (
        source_text.startswith(MARKER)
        and NEW_ADDR_LINE.split("//", 1)[0].rstrip() in source_text
        and NEW_SIZE_LINE.split("//", 1)[0].rstrip() in source_text
        and OLD_ADDR_LINE not in source_text
        and OLD_SIZE_LINE not in source_text
    )


def transform(source):
    """Pure transform: stock InternalFileSystem.cpp text in, relocated text
    out. Raises if the input does not match the exact audited upstream blob,
    or if either target definition is missing/duplicated (a wrong replace
    count) -- never silently patches unrecognized source."""
    if git_blob_sha(source) != ORIGINAL_GIT_BLOB_SHA:
        raise RuntimeError(
            "M7P4 unrecognized InternalFileSystem.cpp; dependency re-audit required")
    source = MARKER + source
    source = replace_once(source, OLD_ADDR_LINE, NEW_ADDR_LINE)
    source = replace_once(source, OLD_SIZE_LINE, NEW_SIZE_LINE)
    return source


def apply(core):
    """Applies the relocation to the real on-disk package at `core` and
    returns a `restore()` callable that reverts it byte-for-byte. The real
    (non-test) invocation below registers that callable with atexit; tests
    call it directly to verify restoration without waiting on process exit.
    """
    if json.loads((core / "package.json").read_text())["version"] != "1.10700.0":
        raise RuntimeError("M7P4 requires Adafruit nRF52 1.7.0 re-audit")

    target, backup = target_and_backup_paths(core)
    source = target.read_text()

    if source.startswith(MARKER):
        # Idempotent recovery from an interrupted prior build: only proceed
        # if a backup exists and re-deriving the patch from it reproduces
        # exactly the content already on disk. Any mismatch fails closed
        # rather than guessing.
        if not backup.exists():
            raise RuntimeError(
                "M7P4 patched InternalFileSystem.cpp found without original backup")
        original = backup.read_text()
        if source != transform(original):
            raise RuntimeError(
                "M7P4 patched InternalFileSystem.cpp altered; dependency re-audit required")
    else:
        original = source
        patched = transform(original)  # Validate exact upstream before writing.
        if backup.exists() and backup.read_text() != original:
            raise RuntimeError(
                "M7P4 InternalFileSystem.cpp backup does not match audited upstream")
        if not backup.exists():
            backup.write_text(original)
        target.write_text(patched)

    def restore():
        try:
            if target.exists() and target.read_text().startswith(MARKER):
                target.write_text(original)
        except Exception:
            # Build result remains authoritative; the next invocation validates
            # marker + backup and fails closed if restoration was interrupted.
            pass

    print("M7P4: verified/applied relocated BLE bond InternalFS patch")
    return restore


if "Import" in globals():
    Import("env")
    atexit.register(apply(Path(env.PioPlatform().get_package_dir("framework-arduinoadafruitnrf52"))))
