"""C1: tasma eklenince gateway izni köprü üzerinden kendiliğinden güncellenir."""
from __future__ import annotations

import http.client
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
from http.server import ThreadingHTTPServer
import unittest

from backend.orun_backend_bench import Backend, oc
from backend.local_observations import LocalObservations
from tools.orun_sunucu import bridge_handler

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import orun_pc_edge as pe  # noqa: E402

GW = 0x09A462BD4B275BA5
TRK1, TRK2 = 0x0E8ADE7E71531AA3, 0x52E16773141A8887
TOKEN = "SYNTHETIC-BRIDGE-TOKEN-1234567890"


class RegistryPendingTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="orun-c1-")
        self.addCleanup(tmp.cleanup)
        self.reg = oc.CredentialRegistry(Path(tmp.name) / "reg.sqlite")
        self.reg.issuer(create=True)

    def grant(self, dev: int) -> None:
        c = self.reg.create(dev)
        self.reg.record_grant(dev, c.key_epoch, GW, 0, 1)

    def test_new_generation_only_when_grants_change(self):
        self.assertIsNone(self.reg.pending_snapshot(GW, 0))          # izin yok, boş paket yok
        self.grant(TRK1)
        gen1, img1 = self.reg.pending_snapshot(GW, 0)
        self.assertEqual(gen1, 1)
        self.assertIsNone(self.reg.pending_snapshot(GW, gen1))      # kurulu: değişiklik yok
        again = self.reg.pending_snapshot(GW, 0)                     # kurulamadı: aynı paket
        self.assertEqual(again, (gen1, img1))
        self.grant(TRK2)                                             # yeni tasma
        gen2, _ = self.reg.pending_snapshot(GW, gen1)
        self.assertEqual(gen2, gen1 + 1)


class BridgeRouteAndInstallTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="orun-c1-http-")
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        self.reg = oc.CredentialRegistry(root / "reg.sqlite")
        self.prefix = oc.issuer_public(self.reg.issuer(create=True))[:4].hex().upper()
        c = self.reg.create(TRK1)
        self.reg.record_grant(TRK1, c.key_epoch, GW, 0, 1)
        backend = Backend(root / "ledger.sqlite", False, registry=self.reg)
        server = ThreadingHTTPServer(("127.0.0.1", 0),
                                     bridge_handler(LocalObservations(backend, "orun-main"), TOKEN))
        server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.base = f"http://127.0.0.1:{server.server_port}"
        self.token_file = root / "tok"
        self.token_file.write_text(TOKEN + "\n")
        self.token_file.chmod(0o600)

    def get(self, path: str, bearer: str | None = TOKEN) -> tuple[int, dict]:
        conn = http.client.HTTPConnection("127.0.0.1", int(self.base.rsplit(":", 1)[1]), timeout=5)
        try:
            conn.request("GET", path, headers={"Authorization": "Bearer " + bearer} if bearer else {})
            r = conn.getresponse()
            return r.status, json.loads(r.read())
        finally:
            conn.close()

    def test_route_needs_token_and_matching_issuer(self):
        q = f"/v1/bridge/ogk1?gateway={GW:016X}&active=0&issuer={self.prefix}"
        self.assertEqual(self.get(q, None)[0], 401)
        self.assertEqual(self.get(q.replace(self.prefix, "00000000"))[0], 409)
        status, body = self.get(q)
        self.assertEqual(status, 200)
        self.assertEqual(body["generation"], 1)
        self.assertEqual(body["lines"][0], "BENCH OGK1 BEGIN")
        self.assertTrue(body["lines"][-1].startswith("BENCH OGK1 COMMIT "))
        self.assertEqual(self.get(q.replace("active=0", "active=1"))[1]["lines"], [])

    def test_pc_edge_installs_newer_snapshot_over_usb(self):
        up = pe.GrantUpdater(self.base, self.token_file, pe.build_opener(pe.ProxyHandler({})))
        up.observe(f"BENCH GATEWAY id={GW:016X} held=0")
        up.observe(f"BENCH OGK1 status=EMPTY generation={0:016X} issuer={self.prefix} installer=IDLE")
        self.assertTrue(up.due(1e9))

        class FakeGateway:
            """Answers like the Gateway bench image; a record line arrives mid-install."""
            def __init__(self):
                self.written: list[str] = []
                self.out = io.BytesIO()
                self.pending: list[bytes] = []
            def write(self, data: bytes):
                line = data.decode().strip()
                self.written.append(line)
                if line.startswith("BENCH OGK1 COMMIT"):
                    self.pending += [b"BENCH OGK1 VERIFIED\n", b"BENCH OGK1 COMMIT PASS\n"]
                else:
                    self.pending += [b"EDGE HELD page=1 slot=2\n", b"BENCH OGK1 OK\n"]
            def read_until(self, _sep, size=4096):
                return self.pending.pop(0) if self.pending else b""

        gw, seen = FakeGateway(), []
        up.poll(gw, 1e9, seen.append)
        self.assertEqual(gw.written[0], "BENCH OGK1 BEGIN")
        self.assertTrue(gw.written[-1].startswith("BENCH OGK1 COMMIT "))
        self.assertIn("EDGE HELD page=1 slot=2", seen)               # kayıtlar beklerken işlendi
        self.assertIsNone(up.status)                                  # gateway yeniden başlıyor


if __name__ == "__main__":
    unittest.main()
