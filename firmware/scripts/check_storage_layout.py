"""Fail the build if the audited core reservation/ownership has changed."""
from pathlib import Path
import importlib.util
import re
import subprocess

Import("env")

core = Path(env.PioPlatform().get_package_dir("framework-arduinoadafruitnrf52"))
linker = env.BoardConfig().get("build.arduino.ldscript")
if linker != "nrf52840_s140_v6.ld":
    raise RuntimeError("M4 requires a storage-layout audit for this linker")
ld = re.sub(r"\s+", "", (core / "cores/nRF5/linker" / linker).read_text())
flash = (core / "libraries/InternalFileSytem/src/flash/flash_nrf5x.c").read_text()
flash_h = (core / "libraries/InternalFileSytem/src/flash/flash_nrf5x.h").read_text()


def _load_patch_internalfs():
    """Load patch_internalfs.py's pure helpers/constants as a plain module
    (no SConscript/Import machinery -- importlib gives it a fresh globals()
    that does not contain "Import", so its own `if "Import" in globals()`
    guard at module scope stays false and apply() is never invoked here).
    Single source of truth for what "correctly M7P4-relocated" means, and
    for the target/backup paths, shared with the post-link ownership check
    below instead of duplicated.
    """
    path = Path(env.subst("$PROJECT_DIR")) / "scripts" / "patch_internalfs.py"
    spec = importlib.util.spec_from_file_location("patch_internalfs", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_patch_internalfs = _load_patch_internalfs()
_internalfs_target, _internalfs_backup = _patch_internalfs.target_and_backup_paths(core)
fs = _internalfs_target.read_text()

# M7P4: the pre-build audit above normally expects the exact stock
# InternalFileSystem.cpp (this pre: script runs before patch_internalfs.py's
# post: application, per PlatformIO's own extra_scripts ordering -- verified
# against platformio's builder/main.py, which runs GetExtraScripts("pre")
# before GetExtraScripts("post")). The one recognized exception is a valid
# M7P4-relocated file left over from an interrupted prior build: the same
# marker/backup verification patch_internalfs.py itself uses, so this script
# does not duplicate or weaken that logic, only reuses it read-only.
_fs_is_stock = bool(re.search(r"#define\s+LFS_FLASH_ADDR\s+0xED000", fs)) and bool(
    re.search(r"#define\s+LFS_FLASH_TOTAL_SIZE\s+\(7\*FLASH_NRF52_PAGE_SIZE\)", fs)
)
_fs_is_valid_leftover_relocation = False
if not _fs_is_stock and _patch_internalfs.relocation_ok(fs):
    _fs_is_valid_leftover_relocation = (
        _internalfs_backup.exists()
        and _patch_internalfs.git_blob_sha(_internalfs_backup.read_text())
        == _patch_internalfs.ORIGINAL_GIT_BLOB_SHA
    )

required = [
    "FLASH(rx):ORIGIN=0x26000,LENGTH=0xED000-0x26000" in ld,
    _fs_is_stock or _fs_is_valid_leftover_relocation,
    re.search(r"#define\s+FLASH_NRF52_PAGE_SIZE\s+4096", flash_h),
    re.search(r"#define\s+BOOTLOADER_ADDR\s+0xF4000", flash),
]
if not all(required):
    raise RuntimeError("M4 flash boundaries changed: audit backend before building")


def check_exclusive_owner(source, target, env):
    nm = Path(env.PioPlatform().get_package_dir("toolchain-gccarmnoneeabi")) / "bin/arm-none-eabi-nm"
    symbols = subprocess.check_output([str(nm), str(target[0])], text=True)
    internalfs_linked = bool(re.search(r"\bInternalFS$", symbols, re.MULTILINE))
    if internalfs_linked:
        # M7P4: a linked InternalFS is no longer an unconditional failure --
        # only a stock or otherwise-unrecognized InternalFS would own the
        # same flash pages as the ORUN history journal. Re-read the actual
        # on-disk vendor source now (patch_internalfs.py's post: application
        # already ran in this same process; its atexit restore has not).
        current = _internalfs_target.read_text() if _internalfs_target.exists() else ""
        if not _patch_internalfs.relocation_ok(current):
            raise RuntimeError(
                "M7P4 ownership violation: InternalFS is linked but its "
                "source does not show the exact M7P4 relocation (base "
                "0x0EB000, 2 pages / 0x2000 bytes). A stock or unrecognized "
                "InternalFS would own the same flash pages as the ORUN "
                "history journal (0xED000..0xF4000)."
            )
    if re.search(r"\bflash_nrf5x_(write|flush)$", symbols, re.MULTILINE) and not internalfs_linked:
        # Only InternalFS's own driver plausibly links these; their presence
        # without InternalFS itself linked is anomalous and still rejected.
        raise RuntimeError("M4 append-only backend must not link the erase/rewrite cache")
    for primitive in ("sd_flash_write", "sd_flash_page_erase"):
        if not re.search(rf"\b{primitive}$", symbols, re.MULTILINE):
            raise RuntimeError(f"M4 backend is missing Nordic primitive {primitive}")


# M7P2 (docs/architecture/ADR_M7_PERSISTENCE_LAYOUT.md): six pages directly
# below history (0x0E7000..0x0ED000) are policy-reserved for the M7P1-decided
# future security/config/BLE-bond partitions. No runtime owner exists for
# them yet; this guard only enforces that the *linked application* never
# occupies that range, so a future implementation slice can allocate it
# without discovering the application has silently grown into it first.
#
# This is a build-time policy guard, not a linker MEMORY-region change: the
# stock, audited nrf52840_s140_v6.ld above still physically permits linking
# all the way to 0x0ED000. Patching the linker itself was deliberately
# deferred (see docs/milestones/M7P2.md) to avoid a third vendor core patch
# before any of the three new partitions has an actual implementation to
# protect. Removing or bypassing this script would remove this protection;
# it is not a hardware-enforced reservation.
STORAGE_CONFIG_H = Path(env.subst("$PROJECT_DIR")) / "include" / "storage_config.h"


def _application_policy_ceiling(storage_config_text):
    """Single source of truth: parse the M7P2 ceiling directly out of
    storage_config.h instead of duplicating the literal in this script.

    kApplicationPolicyEndAddress is itself defined equal to
    kFutureSecurityRegionStart, and a static_assert in that header enforces
    that equality at compile time, so reading the literal-bearing symbol
    here is equivalent to reading the alias without needing a C++ constant
    evaluator in a build script.
    """
    match = re.search(
        r"constexpr\s+uint32_t\s+kFutureSecurityRegionStart\s*=\s*(0[xX][0-9A-Fa-f]+)\s*;",
        storage_config_text,
    )
    if not match:
        raise RuntimeError(
            "M7P2 application ceiling constant kFutureSecurityRegionStart "
            "not found in storage_config.h; audit the layout before building"
        )
    return int(match.group(1), 16)


def _highest_flash_load_end(objdump_section_headers):
    """Pure helper (no PlatformIO/env dependency): given `arm-none-eabi-
    objdump -h` text, return the highest (LMA + size) among sections that
    are actually part of the output image (the LOAD flag).

    This deliberately uses each section's *load* address (LMA), never its
    *virtual* run address (VMA): .data runs from RAM at its VMA but its
    initial contents are stored in flash at its LMA, so its LMA is real
    flash consumption. RAM-only sections such as .bss/.heap are ALLOC but
    not LOAD/CONTENTS -- they consume no flash bytes at all, even though
    the linker still prints a placeholder LMA for them -- so filtering on
    the LOAD flag (not merely "has a nonzero LMA") is required, not
    optional, to avoid over- or under-counting flash usage.
    """
    highest_end = 0
    lines = objdump_section_headers.splitlines()
    header_re = re.compile(
        r"^\s*\d+\s+(\S+)\s+([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)\s+"
        r"([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)\s+\S+\s*$"
    )
    for index, line in enumerate(lines):
        match = header_re.match(line)
        if not match:
            continue
        size = int(match.group(2), 16)
        lma = int(match.group(4), 16)
        flags_line = lines[index + 1] if index + 1 < len(lines) else ""
        flags = {flag.strip() for flag in flags_line.split(",")}
        if "LOAD" not in flags:
            continue
        highest_end = max(highest_end, lma + size)
    return highest_end


def check_application_ceiling(source, target, env):
    ceiling = _application_policy_ceiling(STORAGE_CONFIG_H.read_text())
    objdump = Path(env.PioPlatform().get_package_dir("toolchain-gccarmnoneeabi")) / "bin/arm-none-eabi-objdump"
    headers = subprocess.check_output([str(objdump), "-h", str(target[0])], text=True)
    highest_end = _highest_flash_load_end(headers)
    if highest_end > ceiling:
        raise RuntimeError(
            "M7P2 application flash ceiling exceeded: configured ceiling is "
            f"0x{ceiling:06X}, highest occupied flash byte end is "
            f"0x{highest_end:06X} (exceeds by {highest_end - ceiling} "
            "bytes). This range is policy-reserved for the M7P1 security/"
            "config/bond layout (docs/architecture/ADR_M7_PERSISTENCE_LAYOUT.md)."
        )


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", check_exclusive_owner)
env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", check_application_ceiling)
