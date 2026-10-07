#!/usr/bin/env python3
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MODEL = ROOT / "firmware/scripts/sf4b_custody_capacity.py"
spec = importlib.util.spec_from_file_location("sf4b_capacity", MODEL)
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)

assert module.PAGE_BYTES == 4096
assert module.RECORD_BYTES == 88
assert module.PAGE_HEADER_BYTES == 64
assert module.RECORDS_PER_PAGE == 45
assert module.ADMISSION_PROGRAM_BYTES == 84
assert module.HANDOFF_MARKER_BYTES == 4
assert module.PAGE_HEADER_LIFECYCLE_BYTES == 36
assert module.RECLAIM_INTENT_BYTES == 20

r = module.model(50, 24, 4.0)
assert r.region_bytes == 24 * 4096
assert r.slots == 1080
assert abs(r.distinct_admissions_per_hour - 200.0) < 1e-9
assert abs(r.no_edge_fill_hours - 5.4) < 1e-9
assert abs(r.erases_per_page_year - 1622.2222222222222) < 1e-9
assert abs(r.admission_program_bytes_per_year - (50 * 4 * 8760 * 84)) < 1e-6
assert abs(r.handoff_marker_bytes_per_year - (50 * 4 * 8760 * 4)) < 1e-6
assert abs(
    r.page_header_program_bytes_per_year
    - ((50 * 4 * 8760 / 45) * 36)
) < 1e-6
assert abs(
    r.reclaim_intent_program_bytes_per_year
    - ((50 * 4 * 8760 / 45) * 20)
) < 1e-6

# Opaque-distinct duplicates (for example after tracker reboot/re-protection)
# consume real custody slots/wear. Byte-identical retries do not: they are
# intentionally outside this factor because the store dedupes them in-place.
d = module.model(50, 24, 4.0, opaque_duplicate_fraction=0.10)
assert abs(d.distinct_admissions_per_hour - 220.0) < 1e-9
assert abs(d.no_edge_fill_hours - (1080 / 220.0)) < 1e-9
assert abs(d.erases_per_page_year - r.erases_per_page_year * 1.10) < 1e-9
assert abs(d.total_program_bytes_per_year - r.total_program_bytes_per_year * 1.10) < 1e-6

for args in (
    (0, 24, 4.0, 0.0),
    (50, 1, 4.0, 0.0),
    (50, 24, 0.0, 0.0),
    (50, 24, 4.0, -0.01),
):
    try:
        module.model(*args)
        raise AssertionError(f"invalid model input accepted: {args}")
    except ValueError:
        pass

print("SF4B custody capacity/wear model checks: PASS")
