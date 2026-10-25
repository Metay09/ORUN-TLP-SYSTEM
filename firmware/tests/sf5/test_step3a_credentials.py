#!/usr/bin/env python3
"""Step 3A: real per-Tracker credentials (host only, no device).

- Registry: random credentials, epochs only grow and skip the test epoch,
  one active credential per device, retired ones still open old records,
  0600 file, no reuse of credential ids / roots.
- Shared vectors with the firmware test (test_step3a_credentials.cpp):
  scoped custody ACK key, BENCH PROVISION / BENCH GRANT lines, phone traffic key.
- Provisioning tool against a scripted fake serial link: Tracker (blank,
  test-key refusal, report check, grant recorded) and Gateway (grant lines).
- Backend: opens real-credential records from the registry, not with the
  bench key alone; /v1/edge/keys gives read keys (token required).
- End to end with the app core (Node): the phone fetches read keys from the
  running backend, opens real-credential records and uploads them.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import stat
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
import orun_credentials as oc  # noqa: E402
import orun_edge_bench as eb  # noqa: E402
import orun_provision as op  # noqa: E402
import test_sf5l3_edge_map as l3  # noqa: E402

TRACKER = 0x0E8ADE7E71531AA3
GATEWAY = 0x09A462BD4B275BA5
VECTOR = oc.Credential(TRACKER, bytes(range(0x10, 0x20)), 1, bytes(range(0x40, 0x60)), False)


def test_vectors() -> None:
    assert oc.custody_ack_key(VECTOR, GATEWAY, 0, 1).hex().upper() == \
        "91AE7D108998E87565977EA606FE6A13F6493DEAAF5C46C791BB02B6594A0D60"
    assert oc.traffic_key(VECTOR).hex().upper() == "A432F382698064DB3C7518DF3848C5EF"
    cpp = (ROOT / "firmware/tests/sf5/test_step3a_credentials.cpp").read_text()
    joined = "".join(cpp.split("kProvisionLine[] =")[1].split(";")[0].split('"')[1::2])
    assert joined == oc.provision_line(VECTOR, GATEWAY), joined
    joined = "".join(cpp.split("kGrantLine[] =")[1].split(";")[0].split('"')[1::2])
    assert joined == oc.grant_line(VECTOR, GATEWAY, 0, 1), joined
    # The bench grant source derives the same key for the test credential.
    test = oc.Credential(TRACKER, eb.TEST_CREDENTIAL_ID, eb.TEST_KEY_EPOCH, eb.TEST_ROOT, False)
    assert len(oc.custody_ack_key(test, GATEWAY, 0, 1)) == 32


def test_registry(tmp: Path) -> None:
    reg = oc.CredentialRegistry(tmp / "r.sqlite")
    assert stat.S_IMODE(os.stat(tmp / "r.sqlite").st_mode) == 0o600
    a = reg.create(TRACKER)
    assert a.key_epoch == 1 and len(a.credential_id) == 16 and len(a.k_root) == 32
    try:
        reg.create(TRACKER)
        raise AssertionError("second active credential")
    except oc.RegistryError:
        pass
    b = reg.create(TRACKER, replace=True)
    assert b.key_epoch == 2 and b.credential_id != a.credential_id and b.k_root != a.k_root
    assert reg.active(TRACKER) == b and reg.get(TRACKER, 1).retired
    led = eb.load_ledger_module()
    look = reg.lookup(led)
    assert look(TRACKER.to_bytes(8, "big"), 1).k_root == a.k_root     # old records still open
    assert look(TRACKER.to_bytes(8, "big"), 3) is None
    reg.record_grant(TRACKER, 2, GATEWAY, 0, 1)
    reg.record_grant(TRACKER, 1, GATEWAY, 0, 1)                         # retired: not handed out
    g = reg.grants_for_gateway(GATEWAY)
    assert [(c.key_epoch, f, n) for c, f, n in g] == [(2, 0, 1)]
    # The test epoch is never allocated.
    with reg._conn() as db:
        db.execute("UPDATE credentials SET key_epoch=? WHERE key_epoch=2", (oc.TEST_EPOCH - 1,))
    c = reg.create(TRACKER, replace=True)
    assert c.key_epoch == oc.TEST_EPOCH + 1


class FakeLink:
    """Scripted device: answers to each sent line."""

    def __init__(self, respond):
        self.respond = respond
        self.sent: list[str] = []
        self.pending: list[str] = []

    def send(self, line: str) -> None:
        self.sent.append(line)
        self.pending += self.respond(line)

    def lines(self, seconds: float):
        while self.pending:
            yield self.pending.pop(0)


def tracker_device(state: dict):
    def respond(line: str):
        if line == "BENCH?":
            cred = state.get("credential", "none")
            return ["noise line",
                    f"BENCH CREDENTIAL credential={cred} epoch={state.get('epoch', 0):08X} "
                    f"grant_generation={state.get('gen', 0):08X} custody_floor=00000000",
                    f"BENCH TRACKER id={TRACKER:016X} security=X test_credential=no page=v3 "
                    f"granted_gateway={state.get('gw', 'none')} setup=IDLE role=TRACKER"]
        if line.startswith("BENCH PROVISION"):
            if state.get("credential") not in (None, line.split()[2]):
                return ["BENCH PROVISION FAIL reason=device holds a different credential"]
            state.update(credential=line.split()[2], epoch=int(line.split()[3], 16),
                         gw=line.split()[5], gen=1, line=line)
            return ["BENCH PROVISION started step=CREDENTIAL", "BENCH SETUP step=GRANT",
                    "BENCH SETUP PASS; the pilot sends one stored record now"]
        return []
    return respond


def test_tool(tmp: Path) -> None:
    reg = oc.CredentialRegistry(tmp / "tool.sqlite")
    quiet = lambda *_: None
    # Without --create there is nothing to provision.
    try:
        op.provision_tracker(FakeLink(tracker_device({})), reg, GATEWAY, False, quiet)
        raise AssertionError("provisioned without a credential")
    except op.ProvisionError:
        pass
    state: dict = {}
    link = FakeLink(tracker_device(state))
    rep = op.provision_tracker(link, reg, GATEWAY, True, quiet)
    cred = reg.active(TRACKER)
    assert state["line"] == oc.provision_line(cred, GATEWAY) and rep["generation"] == 1
    assert [(c.device_id, f, g) for c, f, g in reg.grants_for_gateway(GATEWAY)] == [(TRACKER, 0, 1)]
    # Re-run is fine (same credential); a test-key collar is refused untouched.
    op.provision_tracker(FakeLink(tracker_device(state)), reg, GATEWAY, True, quiet)
    test_state = {"credential": eb.TEST_CREDENTIAL_ID.hex().upper(), "epoch": eb.TEST_KEY_EPOCH}
    reg2 = oc.CredentialRegistry(tmp / "tool2.sqlite")
    try:
        op.provision_tracker(FakeLink(tracker_device(test_state)), reg2, GATEWAY, True, quiet)
        raise AssertionError("overwrote the test credential")
    except op.ProvisionError as e:
        assert "CLEAN ALL" in str(e)
    # Gateway: every recorded grant is written, replies checked.
    def gw(line: str):
        if line == "BENCH?":
            return [f"BENCH GATEWAY id={GATEWAY:016X} custody_region=READY custody_ready=yes"]
        if line.startswith("BENCH GRANT"):
            return [f"BENCH GRANT OK added tracker={TRACKER:016X} epoch=00000001 ram_grants=1/8"]
        return []
    glink = FakeLink(gw)
    assert op.grant_gateway(glink, reg, quiet) == 1
    assert glink.sent[1] == oc.grant_line(cred, GATEWAY, 0, 1)
    bad = FakeLink(lambda l: gw(l) if l == "BENCH?" else ["BENCH GRANT FAIL full tracker=x"])
    try:
        op.grant_gateway(bad, reg, quiet)
        raise AssertionError("ignored a failed grant")
    except op.ProvisionError:
        pass


def real_frames(cred: oc.Credential, n: int) -> list[bytes]:
    led = eb.load_ledger_module()
    kat = l3.fixture("kKatPeriodicFrameHex")
    base = bytearray(led.open_product_frame(kat, eb.bench_credentials(led)).plaintext)
    out = []
    for i in range(n):
        p = bytearray(base)
        p[12:16] = (7000 + i).to_bytes(4, "big")
        header = bytearray(kat[:28])
        header[8:16] = cred.device_id.to_bytes(8, "big")
        header[16:20] = cred.key_epoch.to_bytes(4, "big")
        header[20:28] = (900 + i).to_bytes(8, "big")
        from cryptography.hazmat.primitives.ciphers.aead import AESCCM
        nonce = bytes(header[16:20]) + b"\x01" + bytes(header[20:28])
        out.append(bytes(header) + AESCCM(oc.traffic_key(cred), tag_length=8)
                   .encrypt(nonce, bytes(p), bytes(header)))
    return out


def test_backend_and_phone(tmp: Path) -> str:
    reg = oc.CredentialRegistry(tmp / "be.sqlite")
    cred = reg.create(0x1122334455667788)          # the KAT frame's device id
    frames = real_frames(cred, 3)
    bench_only = ob.Backend(tmp / "a.sqlite", True)
    rec = {"frame": frames[0].hex(), "gateway": f"{GATEWAY:016X}", "first_seen_utc": 1_800_000_000}
    assert bench_only.batch({"edge": "e", "records": [rec]})["results"] == ["REJECTED"]
    backend = ob.Backend(tmp / "b.sqlite", True, registry=reg)
    assert backend.batch({"edge": "e", "records": [rec]})["results"] == ["STORED"]
    server = ob.make_server(backend, "127.0.0.1", 0, "TOKEN-1234")
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"
    try:
        req = urllib.request.Request(base + "/v1/edge/keys", headers={"Authorization": "Bearer nope"})
        try:
            urllib.request.urlopen(req, timeout=5)
            raise AssertionError("keys without token")
        except urllib.error.HTTPError as e:
            assert e.code == 401
        req = urllib.request.Request(base + "/v1/edge/keys", headers={"Authorization": "Bearer TOKEN-1234"})
        keys = json.loads(urllib.request.urlopen(req, timeout=5).read())["keys"]
        assert keys == [{"device": "1122334455667788", "epoch": cred.key_epoch,
                         "traffic_key": oc.traffic_key(cred).hex().upper()}]
        assert "k_root" not in json.dumps(keys) and cred.k_root.hex().upper() not in json.dumps(keys)
        if shutil.which("node") is None or not (APP / "node_modules").exists():
            return "phone end-to-end SKIPPED (node or edge/app/node_modules missing)"
        proc = subprocess.run(
            ["node", "--no-warnings", "--experimental-strip-types", "test/keys_e2e.ts"],
            cwd=APP, input=json.dumps({"url": base, "token": "TOKEN-1234",
                                       "frames": [f.hex() for f in frames]}),
            capture_output=True, text=True, timeout=300)
        if proc.returncode != 0:
            raise SystemExit("node failed:\n" + proc.stderr)
        out = json.loads(proc.stdout)
        # before the keys: refused (stays on the Gateway); after: opened
        assert out["states"] == ["RED", "YENİ", "YENİ", "YENİ"], out["states"]
        assert out["run"] == {"sent": 3, "final": 3, "uncertain": 0}, out["run"]
        assert backend.ledger.record_count() == 3
        return "phone fetched read keys, opened 3 real-credential records, uploaded them"
    finally:
        server.shutdown()


def main() -> None:
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        test_vectors()
        test_registry(tmp)
        test_tool(tmp)
        e2e = test_backend_and_phone(tmp)
    print(f"Step 3A credentials: registry, shared vectors, provisioning tool, backend, {e2e}: "
          "PASS (host only)")


if __name__ == "__main__":
    main()
