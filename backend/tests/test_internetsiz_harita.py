"""2: sunucu internetsiz harita paketlerini hesap oturumuyla verir."""
from __future__ import annotations

import hashlib
import http.client
import json
import os
from http.server import ThreadingHTTPServer
from pathlib import Path
import tempfile
import threading
import unittest

from backend.account_login import AccountStore
from backend.map_packs import MapPacks
from backend.tests.test_account_device_login import PASSWORD
from tools.maps.make_offline_catalog import write_sidecar
from tools.orun_shared_sync_loopback import prepare
from tools.orun_sunucu import build

FIELDS = dict(title="Aydın–Muğla harita", kind="protomaps-v4-vector",
              bounds=[27.0, 36.3, 29.9, 38.3], min_zoom=0, max_zoom=15,
              attribution="© OpenStreetMap katkıcıları · Protomaps",
              license_name="ODbL Produced Work")


def pack(directory: Path, version: str, size: int = 3_000_000) -> tuple[Path, bytes]:
    data = b"PMTiles\x03" + bytes((i * 7) & 0xFF for i in range(size - 8))
    path = directory / f"aydin-mugla-{version}.pmtiles"
    path.write_bytes(data)
    write_sidecar(path, pack_id="aydin-mugla", version=version, **FIELDS)
    return path, data


class HaritaPaketleriTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="orun-harita-")
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        state = root / "state"
        state.mkdir(mode=0o700)
        prepare(state, reuse_state=False)
        accounts_db = state / "owner_accounts.sqlite"
        AccountStore(accounts_db).bootstrap_owner("orun-main", "owner", "ownerlogin", PASSWORD)
        os.chmod(accounts_db, 0o600)
        self.maps = root / "maps"
        self.maps.mkdir()
        _, handler = build(state, root / "ledger.sqlite", root / "registry.sqlite",
                           "orun-main", maps_dir=self.maps)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.server.daemon_threads = True
        t = threading.Thread(target=self.server.serve_forever, daemon=True)
        t.start()
        self.addCleanup(t.join, 5)
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)

    def get(self, path: str, bearer: str | None = None) -> tuple[int, dict, bytes]:
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=10)
        headers = {"Authorization": "Bearer " + bearer} if bearer else {}
        try:
            conn.request("GET", path, headers=headers)
            reply = conn.getresponse()
            return reply.status, dict(reply.getheaders()), reply.read()
        finally:
            conn.close()

    def login(self) -> str:
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=10)
        try:
            conn.request("POST", "/api/v1/account/login",
                         body=json.dumps({"username": "ownerlogin", "password": PASSWORD}),
                         headers={"Content-Type": "application/json"})
            reply = conn.getresponse()
            self.assertEqual(reply.status, 200)
            return json.loads(reply.read())["account_access_token"]
        finally:
            conn.close()

    def test_owner_lists_and_downloads_the_pack(self):
        _, data = pack(self.maps, "20261031")
        self.assertEqual(self.get("/api/v1/maps/catalog")[0], 401)
        self.assertEqual(self.get("/api/v1/maps/file/aydin-mugla-20261031.pmtiles")[0], 401)
        token = self.login()

        status, _, raw = self.get("/api/v1/maps/catalog", token)
        self.assertEqual(status, 200)
        catalog = json.loads(raw)
        self.assertEqual(catalog["schema"], "orun-offline-packs-v1")
        [entry] = catalog["packs"]
        self.assertEqual(entry["url"], "/api/v1/maps/file/aydin-mugla-20261031.pmtiles")
        self.assertEqual(entry["bytes"], len(data))
        self.assertEqual(entry["sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(entry["maxZoom"], 15)

        status, headers, body = self.get(entry["url"], token)
        self.assertEqual(status, 200)
        self.assertEqual(headers["Content-Type"], "application/octet-stream")
        self.assertEqual(int(headers["Content-Length"]), len(data))
        self.assertEqual(body, data)

    def test_only_listed_files_are_served(self):
        path, _ = pack(self.maps, "20261031")
        (self.maps / "notes.txt").write_text("x")
        token = self.login()
        for bad in ("../state/owner_accounts.sqlite", "notes.txt",
                    "aydin-mugla-20261030.pmtiles", "%2e%2e/x.pmtiles"):
            self.assertEqual(self.get("/api/v1/maps/file/" + bad, token)[0], 404, bad)
        # Yarım kalmış kopya: boyut tutmuyor → katalogda yok, indirilemez.
        with path.open("ab") as f:
            f.write(b"x")
        self.assertEqual(json.loads(self.get("/api/v1/maps/catalog", token)[2])["packs"], [])
        self.assertEqual(self.get("/api/v1/maps/file/" + path.name, token)[0], 404)

    def test_newest_versions_are_listed_and_missing_dir_is_empty(self):
        pack(self.maps, "20261024", 2000)
        pack(self.maps, "20261031", 3000)
        self.assertEqual([p["version"] for p in MapPacks(self.maps).catalog()["packs"]],
                         ["20261024", "20261031"])
        self.assertEqual(MapPacks(self.maps / "yok").catalog()["packs"], [])


if __name__ == "__main__":
    unittest.main()
