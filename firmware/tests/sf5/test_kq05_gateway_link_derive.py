#!/usr/bin/env python3
"""KQ05 OGK1 v2 Edge/Gateway shared derivation vector; no live credentials."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "edge" / "candidates"))

from kq05_gateway_link_derive import derive_gateway_offer_key


def main():
    master = bytes(range(0x40, 0x60))
    actual = derive_gateway_offer_key(master, b"GATEWAY1", "edge-one")
    # Must also match native C++ signed OGK1 reader derivation test.
    assert actual.hex() == (
        "cfa987ef239b763e3dc1b05d3f7bd3bda8c675659ab0c1935d386e1e466ba84f"
    )
    assert actual != master
    assert derive_gateway_offer_key(master, b"GATEWAY2", "edge-one") != actual
    assert derive_gateway_offer_key(master, b"GATEWAY1", "edge-two") != actual
    for args in [
        (b"", b"GATEWAY1", "edge-one"),
        (b"\x00" * 32, b"GATEWAY1", "edge-one"),
        (master, b"", "edge-one"),
        (master, b"GATEWAY1", "invalid space"),
        (master, b"GATEWAY1", ""),
    ]:
        try:
            derive_gateway_offer_key(*args)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid registration must be refused")
    print("KQ05 OGK1 v2 Edge link derivation cross-language vector: PASS (HOST ONLY)")


if __name__ == "__main__":
    main()
