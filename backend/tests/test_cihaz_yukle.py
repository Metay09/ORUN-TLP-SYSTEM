"""Y1: yükleme öncesi/sonrası cihaz kontrolü (tools/orun_cihaz_kontrol.py)."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "orun_cihaz_kontrol", Path(__file__).resolve().parents[2] / "tools" / "orun_cihaz_kontrol.py")
k = importlib.util.module_from_spec(spec)
spec.loader.exec_module(k)

# The lines the bench images print (firmware/src/main.cpp, sf5l0_bench_gateway.cpp).
TRACKER = [
    "BENCH CREDENTIAL credential=00112233445566778899AABBCCDDEEFF epoch=00000002 grant_generation=00000003 custody_floor=00000000",
    "BENCH TRACKER id=0E8ADE7E71531AA3 security=PROVISIONED test_credential=no page=v3 granted_gateway=09A462BD4B275BA5 setup=DONE role=TRACKER product_store=mounted",
    "PRODUCT STORE ready=yes faulted=no inc=1122334455667788 next_seq=812 retained=40 released=772",
    "APP DEVICE id=4 fw=0.5.0-alpha rev=2 uptime_ms_mod32=1 reset=0x00000001 watchdog=no role=0 role_mode=AUTO relay_applied=no ",
]
GATEWAY = [
    "BENCH GATEWAY id=09A462BD4B275BA5 custody_region=READY custody_ready=yes held=12/1056 role=BASE",
    "BENCH GATEWAY custody_started=40 duplicates=0 denied=0 ack_tx=40 grants_issued=40 grants_refused=0 ram_grants=0 handed_off=28 reclaimed=0",
    "BENCH OGK1 status=READY generation=0000000000000007 issuer=AB12CD34 installer=IDLE signed_acks=40",
    "APP DEVICE id=1 fw=0.5-7b7c540 rev=2 uptime_ms_mod32=1",
]


class Kontrol(unittest.TestCase):
    def test_reads_only_public_facts(self):
        t = k.parse(TRACKER)
        self.assertEqual((t["kind"], t["id"], t["epoch"], t["store_inc"], t["next_seq"], t["fw"]),
                         ("tracker", "0E8ADE7E71531AA3", "00000002", "1122334455667788", 812, "0.5.0-alpha"))
        g = k.parse(GATEWAY)
        self.assertEqual((g["kind"], g["held"], g["capacity"], g["ogk1_generation"], g["fw"]),
                         ("gateway", 12, 1056, "0000000000000007", "0.5-7b7c540"))
        self.assertNotIn("root", str(t))

    def test_same_device_same_key_same_records_passes(self):
        before = k.parse(TRACKER)
        after = k.parse([l.replace("0.5.0-alpha", "0.5-7b7c540").replace("next_seq=812", "next_seq=813") for l in TRACKER])
        res = k.compare(before, after)
        self.assertTrue(all(ok for ok, _ in res), res)
        self.assertIn("Yazılım: 0.5-7b7c540", res[-1][1])
        g = k.parse(GATEWAY)
        self.assertTrue(all(ok for ok, _ in k.compare(g, g)))

    def test_lost_key_records_or_other_device_fail(self):
        before = k.parse(TRACKER)
        for change in ("credential=00112233445566778899AABBCCDDEEFF", "inc=1122334455667788",
                       "next_seq=812", "id=0E8ADE7E71531AA3"):
            bad = {"credential=00112233445566778899AABBCCDDEEFF": "credential=none",
                   "inc=1122334455667788": "inc=99AABBCCDDEEFF00",
                   "next_seq=812": "next_seq=10",
                   "id=0E8ADE7E71531AA3": "id=0E8ADE7E7153FFFF"}[change]
            after = k.parse([l.replace(change, bad) for l in TRACKER])
            self.assertFalse(all(ok for ok, _ in k.compare(before, after)), change)
        g = k.parse(GATEWAY)
        lost = k.parse([l.replace("held=12/1056", "held=0/1056") for l in GATEWAY])
        self.assertFalse(all(ok for ok, _ in k.compare(g, lost)))
        grant = k.parse([l.replace("generation=0000000000000007", "generation=0000000000000000") for l in GATEWAY])
        self.assertFalse(all(ok for ok, _ in k.compare(g, grant)))


if __name__ == "__main__":
    unittest.main()
