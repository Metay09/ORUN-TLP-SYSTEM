#!/usr/bin/env python3
"""Safety regression for ORUN product release gate. No serial port or hardware."""
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

MODULE = Path(__file__).resolve().parents[3] / "tools" / "ci" / "orun_product_release_gate.py"
spec = importlib.util.spec_from_file_location("orun_product_release_gate", MODULE)
gate = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = gate
spec.loader.exec_module(gate)

NORMAL = """[platformio]
default_envs = rak4630
[env:rak4630]
build_flags =
    -I include
[env:rak4630_sf5l0_tracker_bench]
extends = env:rak4630
build_flags =
    -DORUN_SF5K3V_TRACKER_TX_PILOT
    -DORUN_SF5L0_BENCH_TEST_KEYS
    -DORUN_SF5L0_TRACKER
[env:rak4630_sf5l0_gateway_bench]
extends = env:rak4630
build_flags =
    -DORUN_SF5L0_BENCH_TEST_KEYS
    -DORUN_SF5L0_GATEWAY
"""
SNAPSHOT = """ROLE TRACKER mode=AUTO
APP TRACKING id=3 requested_s=180 applied_s=180 effective_s=180 cadence=0 gnss_state=11 detected=yes extra_fix=no attempts=6 fresh=6 timeouts=0 invalid=36 last_ttff_ms=8279 config_ready=yes source=default
PRODUCT STORE region=OK erased=0 observation=0 custody=0 unknown=32 owner=OBSERVATION decision=NEEDS_PREPARATION mounted=no preparing=no
APP STORAGE id=4 history_ready=yes busy=no count=12 capacity=728 overwritten=0 append_failures=0 corrupt=0 metadata_failures=0 config_ready=yes
"""
SHA = "a" * 64


class ProductCutoverGateTests(unittest.TestCase):
    def repo(self, root: Path, env: str = NORMAL) -> None:
        f = root / "firmware"
        (f / "src").mkdir(parents=True)
        (f / "platformio.ini").write_text(env, encoding="utf-8")
        (f / "src" / "main.cpp").write_text(
            "TrackerProductTxPilot foo; GatewayProductCustodyRuntime bar;\n",
            encoding="utf-8")

    def test_bench_flags_must_not_be_counted_as_normal_firmware(self):
        with tempfile.TemporaryDirectory() as p:
            root = Path(p)
            self.repo(root)
            got = {c.key: c for c in gate.check_sources(root)}
            self.assertTrue(got["no-public-test-key"].ok)
            self.assertFalse(got["tracker-secure-tx"].ok)
            self.assertFalse(got["gateway-durable-custody"].ok)
            self.assertFalse(got["secure-commissioning"].ok)

    def test_public_test_key_in_normal_image_is_rejected(self):
        with tempfile.TemporaryDirectory() as p:
            root = Path(p)
            self.repo(root, NORMAL.replace("-I include",
                     "-I include\n    -DORUN_SF5L0_BENCH_TEST_KEYS"))
            self.assertFalse({c.key: c for c in gate.check_sources(root)}
                             ["no-public-test-key"].ok)

    def test_new_real_tracker_capture_blocks_flash_release(self):
        results = {c.key: c for c in gate.check_tracker_snapshot(SNAPSHOT)}
        self.assertTrue(results["tracker-role"].ok)
        self.assertTrue(results["gnss-real-fixes"].ok)
        self.assertTrue(results["history-ready"].ok)
        self.assertFalse(results["product-region-mounted"].ok)
        self.assertFalse(results["security-provisioned"].ok)
        self.assertFalse(results["real-gateway-ack"].ok)
        self.assertIn("unknown=32", results["product-region-mounted"].detail)

    def test_evidence_must_name_every_hop_same_record_and_faults(self):
        record = "0102030405060708:42"
        steps = ("tracker_tx", "gateway_durable", "gateway_ack",
                 "tracker_release", "edge_sqlite", "backend_verified",
                 "android_map")
        faults = ("ack_lost_retry", "duplicate", "tracker_reboot",
                  "gateway_reboot", "offline_edge", "power_cut")
        good = dict(tracker_id="0102030405060708",
                    gateway_id="1122334455667788", firmware_sha256=SHA,
                    record_identity=record, public_test_keys=False,
                    steps={k: {"record_identity": record, "log_sha256": SHA}
                           for k in steps},
                    fault_tests={k: SHA for k in faults})
        with tempfile.TemporaryDirectory() as p:
            manifest = Path(p) / "evidence.json"
            manifest.write_text(json.dumps(good))
            self.assertTrue(gate.check_physical_evidence(manifest)[0].ok)
            good["steps"]["android_map"]["record_identity"] = "other"
            manifest.write_text(json.dumps(good))
            self.assertFalse(gate.check_physical_evidence(manifest)[0].ok)
            good["steps"]["android_map"]["record_identity"] = record
            good["public_test_keys"] = True
            manifest.write_text(json.dumps(good))
            self.assertFalse(gate.check_physical_evidence(manifest)[0].ok)

    def test_main_returns_blocked_and_makes_no_device_calls(self):
        with tempfile.TemporaryDirectory() as p:
            root = Path(p)
            self.repo(root)
            with redirect_stdout(io.StringIO()) as output:
                status = gate.main(["--root", str(root)])
            self.assertEqual(status, 2)
            self.assertIn("BLOCKED", output.getvalue())
            with redirect_stdout(io.StringIO()) as output:
                status = gate.main(["--root", str(root), "--json"])
            self.assertEqual(status, 2)
            self.assertFalse(json.loads(output.getvalue())["release_ready"])


if __name__ == "__main__":
    unittest.main()
