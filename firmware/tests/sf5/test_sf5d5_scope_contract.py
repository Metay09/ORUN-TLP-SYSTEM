#!/usr/bin/env python3
"""SF5D5 scope: the product region backend exists, no runtime writer yet.

SF5E physical qualification must come before main.cpp composes a store on the
region (docs/milestones/SF5D.md).
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]


def code(path: Path) -> str:
    text = path.read_text()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//.*?$", "", text, flags=re.MULTILINE)


main = code(root / "src" / "main.cpp")
for forbidden in ("productPort", "product_region", "NrfProductFlash"):
    assert forbidden not in main, f"main.cpp composes the product region: {forbidden}"

# Only the gate owns the product backend instance.
users = [p.name for p in (root / "src").glob("*.cpp")
         if "NrfProductFlash" in code(p)]
assert sorted(users) == ["nrf_product_flash.cpp"], users
gate_h = code(root / "include" / "flash_mutation_gate.h")
assert "NrfProductFlash sync_product_;" in gate_h
assert re.search(r"kHistory\s*=\s*1,\s*kProduct\s*=\s*2,\s*kConfig\s*=\s*3",
                 gate_h), "Product must be admitted after History, before Config"

print("SF5D5 product region scope contract: PASS")
