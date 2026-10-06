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

r = module.model(50, 24, 4.0)
assert r.region_bytes == 24 * 4096
assert r.slots == 1080
assert abs(r.no_edge_fill_hours - 5.4) < 1e-9
assert abs(r.erases_per_page_year - 1622.2222222222222) < 1e-9

try:
    module.model(0, 24, 4.0)
    raise AssertionError("invalid device count accepted")
except ValueError:
    pass

print("SF4B custody capacity/wear model checks: PASS")
