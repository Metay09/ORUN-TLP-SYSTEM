#!/usr/bin/env python3
"""SF5D4 test-only full-region DFU probes: separation and write limits.

Source checks only; no physical flash claim. The post-link symbol gate in
check_storage_layout.py additionally rejects an erase primitive in both images
and a write primitive in the verifier.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[3]
sf5 = root / "firmware/tests/sf5"
header = (sf5 / "sf5d_full_region_shared.h").read_text()
seed = (sf5 / "sf5d_full_seed.cpp").read_text()
verify = (sf5 / "sf5d_full_verify.cpp").read_text()
remote = (sf5 / "sf5d_full_retention_remote.sh").read_text()
pio = (root / "firmware/platformio.ini").read_text()
production = (root / "firmware/src/main.cpp").read_text()
guard = (root / "firmware/scripts/check_storage_layout.py").read_text()

# Region: exactly the reserved product region, ending at the geofence region.
for required in ("kStart = orun_tlp::storage_config::kProductRegionStart",
                 "kEnd = orun_tlp::storage_config::kProductRegionEnd",
                 "kStart == 0x0C5000U",
                 "kEnd == orun_tlp::storage_config::kGeofenceRegionStart",
                 "kPages == 32U", "pageCrc32(address)",
                 "digest=0x%08lX"):
    assert required in header, f"full-region header missing {required}"

# No mutation in the shared header or the verifier.
for text in (header, verify):
    for forbidden in ("sd_flash_write(", "sd_flash_page_erase(",
                      "nrfx_nvmc_page_erase(", "nrf_nvmc_page_erase(",
                      "NVMC->"):
        assert forbidden not in text, f"read-only source contains {forbidden}"
assert "SF5D FULL SEED CONFIRM" not in verify

# Seed: word writes only, only to fully erased pages, only on command.
assert seed.count("sd_flash_write(") == 1
for forbidden in ("sd_flash_page_erase(", "nrfx_nvmc", "nrf_nvmc", "NVMC->"):
    assert forbidden not in seed, f"seed contains {forbidden}"
assert "if (!pageErased(address)) {" in seed
assert seed.index("if (!pageErased(address)) {") < seed.index("sd_flash_write(")
assert "expectedWords(address, marker)" in seed
assert "!markerMatches(address)" in seed
assert 'acceptLine(command, "SF5D FULL SEED CONFIRM")' in seed
assert "physicalSafety()" in seed
assert "sd_softdevice_is_enabled(&enabled)" in seed
setup = seed.split("void setup()", 1)[1].split("void loop()", 1)[0]
assert "seedAll()" not in setup, "seeding must not run at boot"

# Remote procedure: production image is the update under test, then restore.
assert "flash rak4630_sf5d_full_seed" in remote
assert 'flash rak4630 "production (guncelleme testi)"' in remote
assert "flash rak4630_sf5d_full_verify" in remote
assert "SF5D FULL SEED CONFIRM" in remote
assert '*" erased=0 "*' in remote
assert remote.rstrip().splitlines()[-1].startswith("echo")

assert "sf5d_full" not in production

for name, source in (("rak4630_sf5d_full_seed", "sf5d_full_seed.cpp"),
                     ("rak4630_sf5d_full_verify", "sf5d_full_verify.cpp")):
    env = f"[env:{name}]"
    assert env in pio, f"missing {env}"
    section = pio.split(env, 1)[1].split("\n[env:", 1)[0]
    assert source in section
    assert "pre:scripts/check_storage_layout.py" in section
    assert "-<*>" in section
    assert "lib_deps =" in section
assert '"rak4630_sf5d_full_seed": "seed"' in guard
assert '"rak4630_sf5d_full_verify": "verify"' in guard

print("SF5D4 full-region DFU probe source contracts: PASS")
