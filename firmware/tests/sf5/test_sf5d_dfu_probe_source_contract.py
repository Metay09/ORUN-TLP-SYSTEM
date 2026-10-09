#!/usr/bin/env python3
"""SF5D test-only DFU probe separation; no physical flash claim."""
from pathlib import Path

root = Path(__file__).resolve().parents[3]
header = (root / "firmware/tests/sf5/sf5d_dfu_sentinel_shared.h").read_text()
seed = (root / "firmware/tests/sf5/sf5d_dfu_seed.cpp").read_text()
verify = (root / "firmware/tests/sf5/sf5d_dfu_verify.cpp").read_text()
pio = (root / "firmware/platformio.ini").read_text()
production = (root / "firmware/src/main.cpp").read_text()
guard = (root / "firmware/scripts/check_storage_layout.py").read_text()

for required in ("0x0C5000U", "0x0D5000U", "0x0E4000U",
                 "kGeofenceRegionStart", "markerMatches", "pageErased"):
    assert required in header, f"sentinel contract missing {required}"

for text in (header, verify):
    for forbidden in ("sd_flash_write(", "sd_flash_page_erase(",
                      "nrfx_nvmc_page_erase(", "nrf_nvmc_page_erase("):
        assert forbidden not in text, f"read-only source contains {forbidden}"

assert "sd_flash_write(" in seed
assert "sd_flash_page_erase(" not in seed
assert 'acceptLine(command, "SF5D SEED CONFIRM")' in seed
assert "if (!pageErased(kSeedAddress))" in seed
assert 'kSeedAddress = 0x0E4000U' in header
assert 'kSeedAddress == kAddresses[2]' in header
assert "expectedWords(kSeedAddress, marker)" in seed
assert "reinterpret_cast<uint32_t*>(kSeedAddress)" in seed
assert "pageCrc32(address)" in header
assert "crc32=0x%08lX" in header
assert 'seed_state=%s' in header
assert "for (unsigned i = 0; i < kSampleCount; ++i)" not in seed
assert "kAddresses[0]" not in seed and "kAddresses[1]" not in seed
assert "physicalSafety()" in seed
assert "sd_softdevice_is_enabled(&enabled)" in seed
assert "SF5D SEED CONFIRM" not in verify
assert "sf5d_dfu_sentinel" not in production

for name, source in (
    ("rak4630_sf5d_dfu_seed", "sf5d_dfu_seed.cpp"),
    ("rak4630_sf5d_dfu_verify", "sf5d_dfu_verify.cpp"),
):
    env = f"[env:{name}]"
    assert env in pio
    section = pio.split(env, 1)[1].split("\n[env:", 1)[0]
    assert source in section
    assert "pre:scripts/check_storage_layout.py" in section
    assert "-<*>" in section
    assert "lib_deps =" in section

# The existing universal M4 guard remains mandatory outside these two named
# test images; the seed and read-only verify images have narrower symbol gates.
assert 'env.get("PIOENV", "")' in guard
assert '"rak4630_sf5d_dfu_seed": "seed"' in guard
assert '"rak4630_sf5d_dfu_verify": "verify"' in guard
assert 'sf5d_mode == "seed" and not has_write' in guard
assert 'sf5d_mode == "verify" and has_write' in guard
assert 'SF5D DFU probe must not link Nordic page erase' in guard
assert 'for primitive in ("sd_flash_write", "sd_flash_page_erase"):' in guard
assert 'raise RuntimeError(f"M4 backend is missing Nordic primitive {primitive}")' in guard
assert 'env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", check_application_ceiling)' in guard

print("SF5D DFU sentinel test-only source contracts: PASS")
