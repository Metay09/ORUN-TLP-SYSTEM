#!/usr/bin/env python3
"""SF5L7: the phone app's TypeScript Edge core equals the Python reference.

Host only, no phone. Runs edge/app/test/crosscheck.ts under Node and
compares, on the same inputs:

- frame open + canonical checks: thousands of authentic frames with random
  plaintext mutations (both sides must agree on accept/reject and content),
  plus tampered ciphertext;
- the Edge accept MAC and USB accept line for random (gw, page, slot, gen);
- a full USB session (RX signal pairing, held records, duplicates, a
  truncated line, accept replies OK/BUSY/REJECTED reason=..., reclaim,
  dump end): every printed line, every accept written, the summary and the
  map points (incl. live/through/cut classification).

Skips (exit 0 with a note) only if node or edge/app/node_modules is missing.
"""
from __future__ import annotations

import json
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
APP = ROOT / "edge" / "app"
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import orun_edge_bench as eb  # noqa: E402
import test_sf5l3_edge_map as l3  # noqa: E402

GW = 0x09A462BD4B275BA5


def mutated_frames(rng: random.Random) -> list:
    ledger = eb.load_ledger_module()
    creds = eb.bench_credentials(ledger)
    out = []
    for template_name in ("kKatPeriodicFrameHex", "kKatEventFrameHex"):
        template = l3.fixture(template_name)
        base = ledger.open_product_frame(template, creds).plaintext
        out.append(template)
        for n in range(1500):
            p = bytearray(base)
            for _ in range(rng.choice((1, 1, 2, 3))):
                i = rng.randrange(len(p))
                p[i] = rng.choice((0, 1, 2, 3, 0xFF, rng.randrange(256)))
            out.append(l3.seal(template, bytes(p), 1000 + n))
        for i in (0, 3, 9, 20, 27, 28, 50, len(template) - 1):
            bad = bytearray(template)
            bad[i] ^= 0x04
            out.append(bytes(bad))
    return out


def python_open(frames: list) -> list:
    ledger = eb.load_ledger_module()
    creds = eb.bench_credentials(ledger)
    res = []
    for raw in frames:
        try:
            o = ledger.open_product_frame(raw, creds)
        except ledger.Reject:
            res.append({"ok": False})
            continue
        res.append({
            "ok": True, "device": o.device_id.hex().upper(),
            "incarnation": o.incarnation.hex().upper(), "sequence": o.sequence,
            "family": o.family, "plaintext": o.plaintext.hex().upper(),
            "human": (eb.human(eb.describe_periodic(o.device_id, o.plaintext))
                      .split("\n")[0] if o.family == 1 else None),
        })
    return res


def held(page: int, slot: int, gen: int, raw: bytes) -> str:
    return (f"EDGE HELD v1 gw={GW:016X} page={page} slot={slot} gen={gen:016X} "
            f"len={len(raw)} obj={raw.hex().upper()}")


def usb_script(rng: random.Random, db: Path):
    """Drive the Python BenchEdge, recording the exact inputs it saw."""
    t = [1_790_000_000.0]
    edge = eb.BenchEdge(db, True, clock=lambda: t[0])
    script, outputs, accepts = [], [], []

    def feed(line: str) -> None:
        script.append([t[0], line])
        out = edge.handle_line(line)
        if out is None and line.startswith("EDGE DUMP END"):
            out = f"[gateway] {line} | {edge.summary()}"   # as run_port prints it
        outputs.append(out.split("\n")[0] if out else None)
        a = edge.next_accept(t[0])
        if a:
            accepts.append(a)

    def alive() -> None:
        script.append([t[0], "@alive"])
        edge.session_alive()

    frames = l3.track(14)
    led = eb.load_ledger_module()
    creds = eb.bench_credentials(led)
    rec = [int.from_bytes(led.open_product_frame(f, creds).plaintext[16:20], "big")
           for f in frames]
    t[0] = rec[0] + 2
    alive()
    for i, f in enumerate(frames):
        # live first, then through (<=180 s), then a long gap (late records)
        delay = (2, 3, 1, 20, 4, 100, 150, 120, 600, 5, 5, 5, 5, 5)[i]
        t[0] = max(t[0], rec[i] + delay)
        if i % 3 != 2:
            feed(f"RX PRODUCT_SECURE queued family=1 length=100 rssi={-60 - 3 * i} snr={7 - i}")
            feed("BENCH GW RX record; durable custody write started")
        if i == 5:
            feed("BENCH GW RX duplicate already held")
        feed(held(i // 6, i % 6, 1 + i // 6, f))
        if i == 7:
            feed(held(1, 1, 2, f))                         # dump repeat
            feed(held(1, 2, 2, f)[:-6])                    # truncated line
        if i % 4 == 0:
            alive()
        # Answer whatever accept is in flight, the way a Gateway would.
        while edge._in_flight is not None:
            key = edge._in_flight[0]
            word = ("REJECTED reason=STORE" if i == 3 else
                    rng.choice(("OK", "OK", "OK", "BUSY", "REJECTED reason=MAC")))
            t[0] += 0.5
            if word == "BUSY" and rng.random() < 0.5:
                feed("EDGE ACCEPT BUSY page=? slot=? (store not ready)")
            else:
                feed(f"EDGE ACCEPT {word} page={key[0]} slot={key[1]}")
            if rng.random() < 0.2:
                t[0] += 4                                  # silence: resend
                a = edge.next_accept(t[0])
                script.append([t[0], "@tick"])
                if a:
                    accepts.append(a)
    feed("EDGE RECLAIM page=0 started")
    feed("EDGE RECLAIM page=0 OK")
    feed("EDGE RECLAIM page=1 FAIL")
    feed("EDGE DUMP END count=3")
    alive()
    return script, outputs, accepts, edge.summary(), eb.points_from_db(db)


def run_node(payload: dict) -> dict:
    proc = subprocess.run(
        ["node", "--no-warnings", "--experimental-strip-types", "test/crosscheck.ts"],
        cwd=APP, input=json.dumps(payload), capture_output=True, text=True, timeout=300)
    if proc.returncode != 0:
        raise SystemExit("node failed:\n" + proc.stderr)
    return json.loads(proc.stdout)


def main() -> None:
    if shutil.which("node") is None or not (APP / "node_modules").exists():
        print("SF5L7 Edge app cross-check: SKIPPED (node or edge/app/node_modules "
              "missing; run 'npm ci' in edge/app)")
        return
    rng = random.Random(0x5F57)
    frames = mutated_frames(rng)
    macs = [[f"{rng.getrandbits(64):016X}", rng.randrange(65536), rng.randrange(65536),
             f"{rng.getrandbits(64):016X}", rng.randbytes(rng.choice((84, 100))).hex()]
            for _ in range(200)]
    with tempfile.TemporaryDirectory() as tmp:
        script, outputs, accepts, summary, points = usb_script(rng, Path(tmp) / "e.sqlite")
    payload = {"frames": [f.hex() for f in frames], "macs": macs,
               "script": [[t, line] for t, line in script]}
    ts = run_node(payload)

    py_open = python_open(frames)
    assert len(ts["opened"]) == len(py_open)
    for i, (a, b) in enumerate(zip(py_open, ts["opened"])):
        assert a == b, (i, frames[i].hex(), a, b)
    accepted = sum(1 for o in py_open if o["ok"])
    assert 200 < accepted < len(frames) - 200, accepted   # both outcomes exercised

    for (gw, page, slot, gen, obj), got in zip(macs, ts["macs"]):
        want = eb.edge_accept_mac(int(gw, 16), page, slot, int(gen, 16), bytes.fromhex(obj))
        assert got["mac"] == want.hex().upper()
        assert got["line"] == eb.edge_accept_line(int(gw, 16), page, slot, int(gen, 16),
                                                  bytes.fromhex(obj))

    for i, (a, b) in enumerate(zip(outputs, ts["outputs"])):
        assert a == b, (i, a, b)
    assert len(ts["outputs"]) == len(outputs)
    # #177: incomplete USB dumps must warn on both implementations.
    assert any(line and "UYARI: eksik/bozuk aktarım" in line
               for line in outputs), outputs
    assert ts["accepts"] == accepts, (accepts, ts["accepts"])
    assert ts["summary"] == summary, (summary, ts["summary"])
    assert ts["skipped"] == points["skipped"]
    assert ts["points"] == points["points"], (points["points"], ts["points"])
    links = {p["link"] for p in points["points"]}
    assert {"live", "through"} <= links, links
    assert "teslim_red=0" not in summary and "teslim=0 " not in summary, summary

    print(f"SF5L7 Edge app core = Python reference: {len(frames)} frames "
          f"({accepted} accepted), {len(macs)} accept MACs, USB session of "
          f"{len(script)} lines ({len(accepts)} accepts), points {len(points['points'])} "
          f"links {sorted(str(x) for x in links)}: PASS (host only)")


if __name__ == "__main__":
    main()
