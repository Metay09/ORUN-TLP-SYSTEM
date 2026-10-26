#!/usr/bin/env python3
"""No-network tests for ORUN's authorized PMTiles catalog builder."""
from __future__ import annotations

import importlib.util
import hashlib
from pathlib import Path
import tempfile
import unittest

MODULE = Path(__file__).with_name("make_offline_catalog.py")
spec = importlib.util.spec_from_file_location("offline_catalog_builder", MODULE)
assert spec and spec.loader
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class CatalogBuilderTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.file = Path(self.temp.name) / "aydin.pmtiles"
        self.data = b"PMTiles\x03" + bytes(119)
        self.file.write_bytes(self.data)
        self.args = dict(
            pack_id="aydin-v4", version="2026-10-26", title="Aydın temel harita",
            kind="protomaps-v4-vector",
            bounds=[27.0, 37.0, 29.0, 39.0],
            min_zoom=0, max_zoom=15,
            url="https://maps.example.org/aydin.pmtiles",
            attribution="© OpenStreetMap contributors",
            license_name="ODbL Produced Work",
        )

    def test_local_package_is_hashed_without_network(self) -> None:
        result = module.make_entry(self.file, **self.args)
        self.assertEqual(result["bytes"], len(self.data))
        self.assertEqual(result["sha256"], hashlib.sha256(self.data).hexdigest())
        self.assertEqual(result["bounds"], self.args["bounds"])

    def test_reject_url_without_https(self) -> None:
        with self.assertRaisesRegex(ValueError, "HTTPS"):
            module.make_entry(self.file, **(self.args | {"url": "http://example.org/aydin.pmtiles"}))

    def test_reject_wrong_header(self) -> None:
        self.file.write_bytes(b"something else" + bytes(120))
        with self.assertRaisesRegex(ValueError, "PMTiles"):
            module.make_entry(self.file, **self.args)

    def test_reject_unsupported_maxzoom(self) -> None:
        with self.assertRaisesRegex(ValueError, "max zoom"):
            module.make_entry(self.file, **(self.args | {"max_zoom": 20}))

    def test_reject_non_region_bounds(self) -> None:
        with self.assertRaisesRegex(ValueError, "Invalid bounds"):
            module.make_entry(self.file, **(self.args | {"bounds": [29, 38, 27, 37]}))


if __name__ == "__main__":
    unittest.main()
