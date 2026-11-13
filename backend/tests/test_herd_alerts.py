"""Sürü uyarıları sunucuda: alan dışı, döndü, sessiz/kayıp, pil (herd_alerts.py)."""
from __future__ import annotations

from pathlib import Path
import sqlite3
import tempfile
import unittest

from backend.herd_alerts import (
    BadZones, HerdAlerts, HerdZones, battery_percent, clean_zones, decode, inside,
)
from backend.tests.test_orun_sunucu import TekSunucuTests

NOW = 1_794_600_000            # seconds
MIN = 60
DEV = bytes.fromhex("0E8ADE7E71531AA3")
HEX = DEV.hex().upper()
# A 0.001 degree square around 37.6110, 28.0540 ([lon, lat] like the app).
SQ = [[28.0535, 37.6105], [28.0545, 37.6105], [28.0545, 37.6115], [28.0535, 37.6115]]
IN, OUT = (37.6110, 28.0540), (37.6110, 28.0600)


def rec(ago_min: float, pos=IN, period=600, mv=4000, word=1, power=1, usb=False, hdop=100) -> bytes:
    p = bytearray(64)
    p[1] = 1 | 32 | (2 if pos else 0)
    p[2] = 1
    p[3] = word
    p[16:20] = int(NOW - ago_min * MIN).to_bytes(4, "big")
    p[20:24] = period.to_bytes(4, "big")
    if pos:
        p[24:28] = round(pos[0] * 1e7).to_bytes(4, "big", signed=True)
        p[28:32] = round(pos[1] * 1e7).to_bytes(4, "big", signed=True)
        p[36:38] = hdop.to_bytes(2, "big")
    p[42] = (power << 2) | (0x20 if usb else 0)
    p[56:58] = mv.to_bytes(2, "big")
    return bytes(p)


class HerdAlertTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="orun-uyari-")
        self.addCleanup(tmp.cleanup)
        self.db = Path(tmp.name) / "ledger.sqlite"
        with sqlite3.connect(self.db) as c:
            c.execute("CREATE TABLE verified_observations (device_id BLOB, incarnation BLOB, "
                      "sequence INTEGER, family INTEGER, plaintext BLOB, exact_first_frame BLOB)")
        self.zones = HerdZones(self.db)
        self.alerts = HerdAlerts(self.db, self.zones, clock=lambda: NOW)
        self.seq = 0

    def add(self, *plains: bytes, dev: bytes = DEV) -> None:
        with sqlite3.connect(self.db) as c:
            for p in plains:
                self.seq += 1
                c.execute("INSERT INTO verified_observations VALUES (?,?,?,?,?,?)",
                          (dev, b"\0" * 8, self.seq, 1, p, b""))

    def zone(self, updated_ms: int = 0, **extra) -> None:
        self.zones.set({"zones": [{"id": "a", "name": "Mera", "ring": SQ, "active": True, "updated": updated_ms}],
                        **extra})

    def now(self, names=None, settings=None) -> list[dict]:
        return self.alerts.current({HEX: "Sarıkız"} if names is None else names, settings or {})["alerts"]

    def kinds(self, **kw) -> list[str]:
        return [a["kind"] for a in self.now(**kw)]

    def test_decode_matches_the_bench_reference(self):
        from tools.orun_edge_bench import describe_periodic
        p = rec(5, OUT, period=1200, mv=3876, word=2, power=1)
        ref, got = describe_periodic(DEV, p), decode(p)
        self.assertEqual(got["t"], (NOW - 300) * 1000)
        self.assertEqual((got["period_s"], got["mv"], got["power"], got["usb"]),
                         (ref["period_s"], ref["battery_mv"], ref["power"], ref["usb"]))
        self.assertAlmostEqual(got["lat"] / 1e7, ref["lat"])
        self.assertAlmostEqual(got["lon"] / 1e7, ref["lon"])
        self.assertIsNone(decode(bytes(64)))                 # no trusted time
        self.assertEqual([battery_percent(v) for v in (3400, 3889, 4050, 4190, 4300)], [0, 70, 90, 100, 100])

    def test_geometry_boundary_counts_inside(self):
        v = [(376105000, 280535000), (376105000, 280545000), (376115000, 280545000), (376115000, 280535000)]
        self.assertTrue(inside(v, (376110000, 280540000)))
        self.assertTrue(inside(v, (376105000, 280540000)))   # on an edge
        self.assertFalse(inside(v, (376110000, 280600000)))

    def test_outside_needs_two_of_three_and_names_the_animal(self):
        self.zone()
        self.add(rec(30), rec(20, OUT), rec(10))
        self.assertEqual(self.kinds(), [])                   # one stray fix outside
        self.add(rec(8, OUT), rec(1, OUT))
        (a,) = self.now()
        self.assertEqual((a["kind"], a["title"], a["device"]), ("out", "Sarıkız alan dışına çıktı", HEX))
        self.assertEqual(a["key"], f"out:{HEX}:{(NOW - 20 * MIN) * 1000}")   # first outside fix of the three
        self.assertEqual(self.now(names={})[0]["title"], "ORUN-71531AA3 alan dışına çıktı")
        # A poor fix (HDOP > 5) is not evidence.
        self.add(rec(0.5, IN, hdop=900))
        self.assertEqual(self.kinds(), ["out"])

    def test_no_zone_or_grazing_means_no_outside_alert(self):
        self.add(rec(20, OUT), rec(10, OUT), rec(1, OUT))
        self.assertEqual(self.kinds(), [])
        self.zone(graze={"until": None, "forever": True})
        self.assertEqual(self.kinds(), [])
        self.zone(graze={"until": (NOW + 3600) * 1000, "forever": False})
        self.assertEqual(self.kinds(), [])
        self.zone(graze={"until": (NOW - 60) * 1000, "forever": False})
        self.assertEqual(self.kinds(), ["out"])

    def test_a_zone_drawn_now_does_not_judge_the_past(self):
        self.add(rec(900, OUT), rec(600, OUT), rec(300, OUT), rec(1, OUT))
        self.zone(updated_ms=(NOW - 2 * MIN) * 1000)
        (a,) = self.now()
        self.assertEqual(a["key"], f"out:{HEX}:{(NOW - 2 * MIN) * 1000}")
        self.assertEqual(a["body"], "2 dakikadır açık alanların dışında")
        # A removal is a change too (the app stamps `changed`).
        self.zones.set({"zones": [{"id": "a", "name": "Mera", "ring": SQ, "active": True, "updated": 0}],
                        "changed": (NOW - MIN) * 1000})
        self.assertEqual(self.now()[0]["key"], f"out:{HEX}:{(NOW - MIN) * 1000}")

    def test_back_inside_is_offered_with_the_key_it_answers(self):
        self.zone()
        self.add(rec(60), rec(50, OUT), rec(40, OUT), rec(30, OUT), rec(20), rec(10))
        (a,) = self.now()
        out_key = f"out:{HEX}:{(NOW - 50 * MIN) * 1000}"
        self.assertEqual((a["kind"], a["key"], a["needs"]), ("back", "back:" + out_key, out_key))
        self.assertEqual((a["title"], a["body"]), ("Sarıkız alana döndü", "30 dk dışarıda kaldı"))

    def test_silent_then_lost_by_report_interval(self):
        self.add(rec(25))
        self.assertEqual(self.kinds(), [])                   # 10-min reports: 30 min allowed
        self.alerts.clock = lambda: NOW + 10 * MIN
        (a,) = self.now()
        self.assertEqual((a["kind"], a["title"]), ("silent", "Sarıkız sessiz"))
        self.assertEqual(a["body"], "35 dakikadır konum gelmiyor · 3 rapor kaçtı")
        self.alerts.clock = lambda: NOW + 40 * MIN
        self.assertEqual(self.kinds(), ["lost"])
        # Hourly reports: three hours before anything is said.
        self.setUp()
        self.add(rec(170, period=3600))
        self.assertEqual(self.kinds(), [])
        # Heard without a position is still heard.
        self.setUp()
        self.add(rec(50), rec(5, pos=None))
        self.assertEqual(self.kinds(), [])

    def test_outside_and_quiet_is_lost_at_the_first_limit(self):
        self.zone()
        self.add(rec(60, OUT), rec(50, OUT), rec(40, OUT))
        self.assertEqual(sorted(self.kinds()), ["lost", "out"])

    def test_three_times_faster_outside_then_back_to_base_is_not_silence(self):
        # 20-minute records (base 60 / 3), then nothing for 70 minutes: the collar
        # may have gone back to its hourly base. 80 minutes is silence.
        self.add(rec(110, period=1200), rec(90, period=1200), rec(70, period=1200))
        base = {HEX: {"reported": {"interval_s": 3600}, "reported_utc": NOW - 5 * 3600}}
        self.assertEqual(self.kinds(), ["silent"])           # without the setting: 3 x 20 min
        self.assertEqual(self.kinds(settings=base), [])
        self.alerts.clock = lambda: NOW + 10 * MIN
        self.assertEqual(self.kinds(settings=base), ["silent"])
        # A confirmation newer than the newest record is the interval itself.
        self.alerts.clock = lambda: NOW
        newer = {HEX: {"reported": {"interval_s": 3600}, "reported_utc": NOW - 60 * MIN}}
        self.assertEqual(self.kinds(settings=newer), [])

    def test_battery_critical_once_per_episode_not_on_usb(self):
        self.add(rec(300, mv=3500), rec(200, mv=3410, word=3), rec(5, mv=3405, word=3))
        (a,) = self.now()
        self.assertEqual((a["kind"], a["title"]), ("battery", "Sarıkız: pil bitiyor"))
        self.assertEqual(a["key"], f"battery:{HEX}:{(NOW - 300 * MIN) * 1000 // 86_400_000}")
        self.setUp()
        self.add(rec(5, mv=3405, word=3, power=2, usb=True))  # charging
        self.assertEqual(self.kinds(), [])
        self.setUp()
        self.add(rec(5, mv=3700))
        self.assertEqual(self.kinds(), [])

    def test_zone_book_is_validated(self):
        ok = {"zones": [{"id": "a", "name": " Mera ", "ring": SQ, "active": True, "updated": 5}]}
        self.assertEqual(clean_zones(ok)["zones"][0]["name"], "Mera")
        self.assertEqual(self.zones.set(ok), {"zones": 1, "active": 1})
        self.assertEqual(self.zones.get()["zones"][0]["ring"], SQ)
        for bad in ({}, {"zones": {}}, {"zones": [{"id": "a", "name": "x", "ring": SQ[:2], "active": True}]},
                    {"zones": [{"id": "a", "name": "", "ring": SQ, "active": True}]},
                    {"zones": [{"id": "a", "name": "x", "ring": [[200, 0], [0, 0], [1, 1]], "active": True}]},
                    {"zones": [{"id": "a", "name": "x", "ring": SQ, "active": "yes"}]},
                    {"zones": [ok["zones"][0], ok["zones"][0]]},
                    {"zones": [], "graze": {"until": "soon"}}):
            with self.assertRaises(BadZones):
                clean_zones(bad)


class HerdAlertHttpTests(TekSunucuTests):
    def test_account_reads_alerts_and_stores_zones(self):
        self.assertEqual(self.req(self.account, "GET", "/api/v1/herd/alerts")[0], 401)
        self.assertEqual(self.req(self.account, "POST", "/api/v1/herd/zones", body={"zones": []})[0], 401)
        token = self.login()
        status, body = self.req(self.account, "GET", "/api/v1/herd/alerts", token)
        self.assertEqual(status, 200)
        self.assertEqual(body["alerts"], [])                 # the fixture's records are years old
        self.assertIsInstance(body["now"], int)
        doc = {"zones": [{"id": "a", "name": "Mera", "ring": SQ, "active": True, "updated": 1}]}
        self.assertEqual(self.req(self.account, "POST", "/api/v1/herd/zones", token, doc),
                         (200, {"zones": 1, "active": 1}))
        self.assertEqual(self.req(self.account, "GET", "/api/v1/herd/zones", token)[1]["zones"][0]["ring"], SQ)
        self.assertEqual(self.req(self.account, "POST", "/api/v1/herd/zones", token, {"zones": "x"})[0], 400)


if __name__ == "__main__":
    unittest.main()
