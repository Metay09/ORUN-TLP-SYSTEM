#!/usr/bin/env python3
"""Host-only validation for exact offline GeoJSON -> ORUN USB fence staging."""
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("orun_fence_usb", ROOT / "tools" / "orun_fence_usb.py")
assert spec is not None and spec.loader is not None
fence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fence)


def square(lon=28.0, lat=37.0):
    return [[lon,lat],[lon+0.001,lat],[lon+0.001,lat+0.001],
            [lon,lat+0.001],[lon,lat]]


def record(ring, active=True):
    return {"type":"Feature","properties":{"active":active},
            "geometry":{"type":"Polygon","coordinates":[ring]}}


def book(*records):
    return {"type":"FeatureCollection","features":list(records)}


class FenceUsbTests(unittest.TestCase):
    def test_frozen_commands_and_counts(self):
        poly = fence.parse_geojson(book(record(square())))
        self.assertEqual([len(p) for p in poly],[4])
        steps = fence.commands(poly)
        self.assertEqual(steps[0],("APP FENCE BEGIN",1,0))
        self.assertEqual(steps[1],("APP FENCE V 37.0000000 28.0000000",1,1))
        self.assertEqual(steps[-1],("APP FENCE V 37.0010000 28.0000000",1,4))
        self.assertFalse(any("CLEAR" in s[0] or "COMMIT" in s[0] for s in steps))

    def test_multiple_polygons_one_atomic_replace(self):
        poly = fence.parse_geojson(book(record(square()),record(square(28.01,37.01))))
        self.assertEqual(len(poly),2)
        self.assertIn(("APP FENCE AREA",2,4),fence.commands(poly))
        self.assertEqual(fence.commands(poly)[-1][2],8)

    def test_disabled_only_does_not_clear(self):
        with self.assertRaises(fence.FenceError):
            fence.parse_geojson(book(record(square(),False)))
        p = fence.parse_geojson(book(record(square(),False),record(square(28.1,37.1),True)))
        self.assertEqual(len(p),1)

    def test_holes_and_bowtie_rejected(self):
        hole = record(square())
        hole["geometry"]["coordinates"].append(square(28.0002,37.0002))
        with self.assertRaises(fence.FenceError):
            fence.parse_geojson(book(hole))
        bow = [[28,37],[28.001,37.001],[28.001,37],[28,37.001],[28,37]]
        with self.assertRaises(fence.FenceError):
            fence.parse_geojson(book(record(bow)))

    def test_9_areas_and_65_corners_refused(self):
        with self.assertRaises(fence.FenceError):
            fence.parse_geojson(book(*(record(square(28+i*.01)) for i in range(9))))
        # 8 * 9 corners = 72 total. Polygon vertices remain locally valid.
        import math
        rings = []
        for i in range(8):
            pts = [[28+.001*math.cos(k*2*math.pi/9)+i*.01,
                    37+.001*math.sin(k*2*math.pi/9)] for k in range(9)]
            rings.append(record(pts+[pts[0]]))
        with self.assertRaises(fence.FenceError):
            fence.parse_geojson(book(*rings))

    def test_bad_number_and_invalid_import(self):
        with self.assertRaises(fence.FenceError):
            fence.parse_geojson({"type":"FeatureCollection","features":[]})
        for x in (True,float("inf"),90,-90):
            p = record(square())
            p["geometry"]["coordinates"][0][1] = x
            with self.assertRaises(fence.FenceError):
                fence.parse_geojson(book(p))
        with self.assertRaises(fence.FenceError):
            fence.parse_geojson(book({"type":"Feature","geometry":{"type":"Point","coordinates":[28,37]}}))

    def test_signed_rounding_matches_javascript(self):
        self.assertEqual(fence.as_e7(-0.00000005),0)
        self.assertEqual(fence.as_e7(0.00000005),1)
        self.assertEqual(fence.as_e7(-0.00000015),-1)
        self.assertEqual(fence.as_e7(0.00000015),2)


if __name__ == "__main__":
    unittest.main()
