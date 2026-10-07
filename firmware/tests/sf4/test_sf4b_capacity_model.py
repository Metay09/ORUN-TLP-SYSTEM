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
assert module.PAGE_HEADER_BYTES == 64
assert module.RECORD_BYTES == 92
assert module.RECORDS_PER_PAGE == 43
assert module.INTENT_SLOT_BYTES == 24
assert module.INTENT_SLOTS_PER_PAGE == 3
assert module.INTENT_AREA_BYTES == 72
assert module.ADMISSION_PROGRAM_BYTES == 84
assert module.HANDOFF_MARKER_BYTES == 4
assert module.PAGE_HEADER_LIFECYCLE_BYTES == 40
assert module.RECLAIM_INTENT_BYTES == 24

r = module.model(50, 24, 4.0)
assert r.region_bytes == 24 * 4096
assert r.slots == 1032
assert abs(r.distinct_admissions_per_hour - 200.0) < 1e-9
assert abs(r.no_edge_fill_hours - 5.16) < 1e-9
assert abs(r.erases_per_cycling_page_year - 1697.674418604651) < 1e-9
assert r.duplicate_scan_header_reads == 24
assert r.duplicate_scan_record_reads == 24 * 43
assert r.duplicate_scan_read_bytes == 24 * 64 + 24 * 43 * 92
assert r.duplicate_scan_crc_bytes == 24 * 43 * 76
assert r.recovery_header_reads_worst_case == 24 * 24 + 24
assert r.recovery_record_reads_worst_case == 24 * 43
assert r.recovery_read_bytes_worst_case == (
    (24 * 24 + 24) * 64 + 24 * 43 * 92 + 72
)

d = module.model(
    50, 24, 4.0,
    opaque_duplicate_fraction=0.10,
    late_retry_readmission_fraction=0.05,
)
assert abs(d.distinct_admissions_per_hour - 230.0) < 1e-9
assert abs(
    d.erases_per_cycling_page_year
    - r.erases_per_cycling_page_year * 1.15
) < 1e-9

pinned = module.model(50, 24, 4.0, pinned_pages=4)
assert pinned.cycling_pages == 20
assert abs(
    pinned.erases_per_cycling_page_year
    - r.page_reclaims_per_year / 20
) < 1e-9
assert pinned.erases_per_cycling_page_year > r.erases_per_cycling_page_year

for args in (
    (0, 24, 4.0, 0.0, 0.0, 0),
    (50, 1, 4.0, 0.0, 0.0, 0),
    (50, 24, 0.0, 0.0, 0.0, 0),
    (50, 24, 4.0, -0.01, 0.0, 0),
    (50, 24, 4.0, 0.0, -0.01, 0),
    (50, 24, 4.0, 0.0, 0.0, 24),
):
    try:
        module.model(*args)
        raise AssertionError(f"invalid model input accepted: {args}")
    except ValueError:
        pass

print("SF4B custody capacity/wear/scan model checks: PASS")
