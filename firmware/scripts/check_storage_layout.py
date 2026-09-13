"""Fail the build if the audited core reservation/ownership has changed."""
from pathlib import Path
import re
import subprocess

Import("env")

core = Path(env.PioPlatform().get_package_dir("framework-arduinoadafruitnrf52"))
linker = env.BoardConfig().get("build.arduino.ldscript")
if linker != "nrf52840_s140_v6.ld":
    raise RuntimeError("M4 requires a storage-layout audit for this linker")
ld = re.sub(r"\s+", "", (core / "cores/nRF5/linker" / linker).read_text())
fs = (core / "libraries/InternalFileSytem/src/InternalFileSystem.cpp").read_text()
flash = (core / "libraries/InternalFileSytem/src/flash/flash_nrf5x.c").read_text()
flash_h = (core / "libraries/InternalFileSytem/src/flash/flash_nrf5x.h").read_text()
required = [
    "FLASH(rx):ORIGIN=0x26000,LENGTH=0xED000-0x26000" in ld,
    re.search(r"#define\s+LFS_FLASH_ADDR\s+0xED000", fs),
    re.search(r"#define\s+LFS_FLASH_TOTAL_SIZE\s+\(7\*FLASH_NRF52_PAGE_SIZE\)", fs),
    re.search(r"#define\s+FLASH_NRF52_PAGE_SIZE\s+4096", flash_h),
    re.search(r"#define\s+BOOTLOADER_ADDR\s+0xF4000", flash),
]
if not all(required):
    raise RuntimeError("M4 flash boundaries changed: audit backend before building")


def check_exclusive_owner(source, target, env):
    nm = Path(env.PioPlatform().get_package_dir("toolchain-gccarmnoneeabi")) / "bin/arm-none-eabi-nm"
    symbols = subprocess.check_output([str(nm), str(target[0])], text=True)
    if re.search(r"\bInternalFS$", symbols, re.MULTILINE):
        raise RuntimeError("M4 journal and InternalFS cannot own the same flash pages")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", check_exclusive_owner)
