#!/usr/bin/env python3
"""SF5L8: phone (Edge) -> bench backend upload, host only.

- Backend batch rules: exact frames verified by the backend ledger; STORED,
  DUPLICATE, REJECTED (tampered), CONFLICT (same identity, other content);
  malformed bodies refused before anything is stored; receipts keep the
  earliest arrival and first known signal; sessions are upserted.
- HTTP: wrong token 401, oversize 413, malformed 400, unknown path 404, the
  map page and /api/points.
- End to end: the app's TypeScript core (edge/app) fills a phone ledger over
  BLE messages and uploads it with fetch to this backend running for real;
  the backend map points equal the phone's, a second run sends nothing.
  (Skipped with a note if node or edge/app/node_modules is missing.)
"""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[3]
APP = ROOT / "edge" / "app"
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "backend"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import orun_backend_bench as ob  # noqa: E402
import orun_edge_bench as eb  # noqa: E402
import test_sf5l3_edge_map as l3  # noqa: E402

TOKEN = "ORUN-BENCH-TEST"


def rec(frame: bytes, seen=1_800_000_000, rssi=None, snr=None, gw="09A462BD4B275BA5"):
    return {"frame": frame.hex(), "gateway": gw, "first_seen_utc": seen, "rssi": rssi, "snr": snr}


def test_batch(tmp: Path) -> None:
    b = ob.Backend(tmp / "b.sqlite", True, clock=lambda: 1_800_000_100)
    frames = l3.track(3)
    out = b.batch({"edge": "phone-aa", "records": [rec(frames[0], 1_800_000_050, -90, 4),
                                                    rec(frames[1])],
                   "sessions": [{"id": 1, "started_utc": 10, "alive_utc": 20, "transport": "ble"}]})
    assert out["results"] == ["STORED", "STORED"] and out["records"] == 2 and out["sessions_ok"]
    bad = bytearray(frames[2]); bad[60] ^= 1
    # A second Edge: earlier arrival wins, the first known signal stays.
    out = b.batch({"edge": "phone-bb", "records": [rec(frames[0], 1_800_000_010, -50, 9),
                                                    rec(bytes(bad))],
                   "sessions": [{"id": 1, "started_utc": 10, "alive_utc": 30, "transport": "usb"}]})
    assert out["results"] == ["DUPLICATE", "REJECTED"], out
    # Conflict: same identity, different authenticated content.
    led = eb.load_ledger_module()
    p = bytearray(led.open_product_frame(frames[1], eb.bench_credentials(led)).plaintext)
    p[57] ^= 1
    other = l3.seal(l3.fixture("kKatPeriodicFrameHex"), bytes(p), 9000)
    assert b.batch({"edge": "phone-aa", "records": [rec(other)]})["results"] == ["CONFLICT"]
    # Session upsert keeps the latest alive time per (edge, session).
    b.batch({"edge": "phone-aa", "sessions": [{"id": 1, "started_utc": 10, "alive_utc": 25,
                                                "transport": "ble"}]})
    with sqlite3.connect(str(tmp / "b.sqlite")) as db:
        r = db.execute("SELECT first_seen_utc,gw_rssi,gw_snr FROM edge_receipts ORDER BY sequence").fetchall()
        assert r[0] == (1_800_000_010, -90, 4), r
        s = db.execute("SELECT edge_id,session_id,alive_utc FROM edge_sessions ORDER BY edge_id").fetchall()
        assert s == [("phone-aa", 1, 25), ("phone-bb", 1, 30)], s
        # (device, incarnation, sequence, edge): the conflict repeats phone-aa + f1
        assert db.execute("SELECT count(*) FROM backend_uploads").fetchone()[0] == 3
    # Malformed input never stores anything.
    for body in ({"edge": "x y", "records": []},
                 {"edge": "e", "records": [{"frame": "zz"}]},
                 {"edge": "e", "records": [rec(frames[2]) | {"gateway": "123"}]},
                 {"edge": "e", "records": [rec(frames[2]) | {"rssi": "-5"}]},
                 {"edge": "e", "records": [rec(frames[2])] * (ob.MAX_RECORDS + 1)},
                 {"edge": "e", "sessions": [{"id": 1, "started_utc": 9, "alive_utc": 8}]},
                 []):
        try:
            b.batch(body)
            raise AssertionError(f"accepted {body!r:.80}")
        except ob.BadRequest:
            pass
    assert b.ledger.record_count() == 2


def serve(tmp: Path):
    backend = ob.Backend(tmp / "http.sqlite", True)
    server = ob.make_server(backend, "127.0.0.1", 0, TOKEN)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return backend, server, f"http://127.0.0.1:{server.server_address[1]}"


def call(url: str, body: bytes | None = None, token: str = TOKEN, method: str = "POST"):
    req = urllib.request.Request(url, data=body, method=method,
                                 headers={"Content-Type": "application/json",
                                          "Authorization": f"Bearer {token}"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def test_http(tmp: Path) -> None:
    backend, server, base = serve(tmp)
    try:
        ok = json.dumps({"edge": "phone-cc", "records": [rec(l3.track(1)[0])]}).encode()
        assert call(base + "/v1/edge/batch", ok, token="wrong")[0] == 401
        assert call(base + "/v1/edge/batch", b"x" * (ob.MAX_BODY + 1))[0] == 413
        assert call(base + "/v1/edge/batch", b"{nope")[0] == 400
        assert call(base + "/v1/other", ok)[0] == 404
        empty = json.dumps({"edge": "phone-cc", "records": [], "sessions": []}).encode()
        code, body = call(base + "/v1/edge/batch", empty)   # the app's connection check
        assert code == 200 and json.loads(body)["results"] == []
        code, body = call(base + "/v1/edge/batch", ok)
        assert code == 200 and json.loads(body)["results"] == ["STORED"]
        code, body = call(base + "/api/points", method="GET")
        assert code == 200 and json.loads(body)["count"] == 1
        code, body = call(base + "/", method="GET")
        assert code == 200 and b"loadPoints" in body
        code, body = call(base + "/v1/health", method="GET")
        assert code == 200 and json.loads(body)["records"] == 1
    finally:
        server.shutdown()


def test_end_to_end(tmp: Path) -> str:
    if shutil.which("node") is None or not (APP / "node_modules").exists():
        return "end-to-end SKIPPED (node or edge/app/node_modules missing)"
    backend, server, base = serve(tmp / "e2e")
    try:
        frames = l3.track(130)               # more than one 100-record batch
        led = eb.load_ledger_module()
        creds = eb.bench_credentials(led)
        rec_t = [int.from_bytes(led.open_product_frame(f, creds).plaintext[16:20], "big")
                 for f in frames]
        arrivals = [t + (5 if i < 100 else 900) for i, t in enumerate(rec_t)]
        links = [[-70 - i % 30, 5] if i % 3 == 0 else None for i in range(len(frames))]
        payload = {"url": base, "token": TOKEN, "frames": [f.hex() for f in frames],
                   "links": links, "arrivals": arrivals, "t0": rec_t[0]}
        proc = subprocess.run(
            ["node", "--no-warnings", "--experimental-strip-types", "test/sync_e2e.ts"],
            cwd=APP, input=json.dumps(payload), capture_output=True, text=True, timeout=300)
        if proc.returncode != 0:
            raise SystemExit("node failed:\n" + proc.stderr)
        out = json.loads(proc.stdout)
        assert out["run"] == {"sent": 130, "final": 130, "uncertain": 0}, out["run"]
        assert out["again"]["sent"] == 0, out["again"]
        assert out["counts"] == {"uploaded": 130, "pending": 0, "conflict": 0, "rejected": 0}
        server_points = eb.points_from_db(backend.db_path)["points"]
        assert len(server_points) == 130
        assert server_points == out["phone"], "backend map != phone map"
        links_seen = sorted({str(p["link"]) for p in server_points})
        return f"end-to-end 130 records, backend map == phone map, links {links_seen}"
    finally:
        server.shutdown()


def main() -> None:
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        (tmp / "e2e").mkdir()
        test_batch(tmp)
        test_http(tmp)
        e2e = test_end_to_end(tmp)
    print(f"SF5L8 bench backend + phone upload: batch rules, HTTP, {e2e}: PASS (host only)")


if __name__ == "__main__":
    main()
