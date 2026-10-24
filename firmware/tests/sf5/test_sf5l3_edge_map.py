#!/usr/bin/env python3
"""SF5L3: bench map server over the verified ledger (host only).

Builds a ledger from real SF5B-sealed frames (the on-device KAT plaintext,
re-sealed with the PUBLIC test credential at other sequences/positions),
then checks /api/points, the static whitelist and the page wiring.
"""
from __future__ import annotations

import json
from pathlib import Path
import re
import sys
import tempfile
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
import orun_edge_bench as eb  # noqa: E402

from cryptography.hazmat.primitives import hashes  # noqa: E402
from cryptography.hazmat.primitives.ciphers.aead import AESCCM  # noqa: E402
from cryptography.hazmat.primitives.kdf.hkdf import HKDF  # noqa: E402


def fixture(name: str) -> bytes:
    text = (ROOT / "firmware/tests/sf5/sf5b_product_vectors.h").read_text()
    m = re.search(r"static const char " + re.escape(name) +
                  r"\[\]\s*=\s*((?:\s*\"[0-9a-f]+\"\s*)+);", text)
    assert m is not None, name
    return bytes.fromhex("".join(re.findall(r'"([0-9a-f]+)"', m.group(1))))


def seal(template: bytes, plaintext: bytes, counter: int) -> bytes:
    header = bytearray(template[:28])
    header[20:28] = counter.to_bytes(8, "big")
    epoch = eb.TEST_KEY_EPOCH.to_bytes(4, "big")
    key = HKDF(hashes.SHA256(), 16, eb.TEST_CREDENTIAL_ID,
               b"ORUN-TLP-V2-AEAD" + bytes((1,)) + epoch).derive(eb.TEST_ROOT)
    nonce = epoch + bytes((1,)) + counter.to_bytes(8, "big")
    return bytes(header) + AESCCM(key, tag_length=8).encrypt(nonce, plaintext, bytes(header))


def track(n: int):
    """n canonical PERIODIC plaintexts walking a short path near Çine."""
    kat = fixture("kKatPeriodicFrameHex")
    ledger = eb.load_ledger_module()
    creds = eb.bench_credentials(ledger)
    base = ledger.open_product_frame(kat, creds).plaintext
    frames = []
    for i in range(n):
        p = bytearray(base)
        p[12:16] = (1000 + i).to_bytes(4, "big")                   # sequence
        p[16:20] = (int.from_bytes(base[16:20], "big") + 60 * i).to_bytes(4, "big")
        lat = 376105000 + 40 * i + (15 if i % 2 else -15) * 3
        lon = 280546000 + 55 * i
        p[24:28] = lat.to_bytes(4, "big", signed=True)
        p[28:32] = lon.to_bytes(4, "big", signed=True)
        p[56:58] = (4100 - i).to_bytes(2, "big")                   # battery mV
        frames.append(seal(kat, bytes(p), 500 + i))
    return frames


def get(url: str):
    with urllib.request.urlopen(url, timeout=5) as r:
        return r.status, r.headers.get("Content-Type", ""), r.read()


def main() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        db = Path(tmp) / "ledger.sqlite"
        assert eb.points_from_db(db) == {"count": 0, "skipped": 0, "points": []}

        edge = eb.BenchEdge(db, test_key=True)
        frames = track(12)
        for i, f in enumerate(frames):
            out = edge.handle_line(
                "EDGE HELD v1 gw=09A462BD4B275BA5 page=0 slot=%d "
                "gen=0000000000000001 len=100 obj=%s" % (i, f.hex().upper()))
            assert out and out.startswith("YENİ"), out
        view = eb.points_from_db(db)
        assert view["count"] == 12 and view["skipped"] == 0
        seqs = [p["seq"] for p in view["points"]]
        assert seqs == sorted(seqs) and seqs[0] == 1000
        first = view["points"][0]
        assert first["tracker"] == "1122334455667788"
        assert abs(first["lat"] - 37.6104955) < 1e-6 and abs(first["lon"] - 28.0546) < 1e-6
        assert first["battery_mv"] == 4100 and first["time"].endswith("Z")

        server = eb.make_map_server(db, "127.0.0.1", 0)
        import threading
        threading.Thread(target=server.serve_forever, daemon=True).start()
        base = "http://127.0.0.1:%d" % server.server_address[1]
        try:
            code, ctype, body = get(base + "/api/points")
            assert code == 200 and ctype == "application/json"
            assert json.loads(body)["count"] == 12
            code, ctype, html = get(base + "/")
            assert code == 200 and ctype.startswith("text/html")
            page = html.decode()
            assert "/vendor/leaflet-1.9.4/leaflet.js" in page and "/api/points" in page
            assert "innerHTML" not in page.replace("outerHTML", "")  # text via textContent
            for asset in ("leaflet.js", "leaflet.css", "images/layers.png"):
                code, _, data = get(base + "/vendor/leaflet-1.9.4/" + asset)
                assert code == 200 and len(data) > 100, asset
            for bad in ("/../tools/orun_edge_bench.py", "/vendor/", "/ledger.sqlite",
                        "/%2e%2e/etc/passwd", "/index.html/../../x"):
                try:
                    get(base + bad)
                    raise AssertionError("served " + bad)
                except urllib.error.HTTPError as e:
                    assert e.code == 404, (bad, e.code)
            # Read-only: the server never writes the ledger.
            before = db.stat().st_mtime_ns
            get(base + "/api/points")
            assert db.stat().st_mtime_ns == before
        finally:
            server.shutdown()

    print("SF5L3 bench map server: ledger view, whitelist, page wiring: PASS (host only)")


if __name__ == "__main__":
    main()
