"""1a: PC geçişi — eski servis satırlarından tek sunucu satırı."""
from __future__ import annotations

import shlex
import unittest

from tools.orun_pc_gecis import backend_config, exec_argv, options, shared_config, unit_text

SHOW_BACKEND = (
    "{ path=/usr/bin/python3 ; argv[]=/usr/bin/python3 /home/m/ORUN/backend/orun_backend_bench.py "
    "--db /home/m/orun-backend-bench.sqlite --registry /home/m/orun-credentials.sqlite "
    "--token-file /home/m/.orun/backend_token --host 100.114.36.9 --port 8090 ; "
    "ignore_errors=no ; start_time=[n/a] ; stop_time=[n/a] ; pid=0 ; code=(null) ; status=0/0 }")
SHOW_SHARED = (
    "{ path=/usr/bin/python3 ; argv[]=/usr/bin/python3 -I /home/m/ORUN-RELEASES/observation-a300b8e/"
    "tools/orun_shared_sync_loopback.py --state-dir /home/m/ORUN-SHARED-RUNTIME/state/backend "
    "--reuse-state --enable-account-login --port 18781 --observation-upstream http://100.114.36.9:8090 "
    "--observation-token-file /home/m/.orun/backend_token --observation-site orun-main "
    "--observation-records-upstream http://127.0.0.1:18782 ; ignore_errors=no ; start_time=[n/a] }")


class GecisTests(unittest.TestCase):
    def test_reads_real_settings_and_writes_one_service(self):
        b = backend_config(exec_argv(SHOW_BACKEND))
        s = shared_config(exec_argv(SHOW_SHARED))
        self.assertEqual((b["host"], b["port"], b["bench"]), ("100.114.36.9", 8090, False))
        self.assertEqual(s, {"state_dir": "/home/m/ORUN-SHARED-RUNTIME/state/backend",
                             "site": "orun-main", "port": 18781})
        unit = unit_text("/home/m/ORUN-RELEASES/sunucu-abc1234", "/usr/bin/python3", b, s,
                         "/home/m/.orun/backend_token")
        line = next(x for x in unit.splitlines() if x.startswith("ExecStart="))
        o = options(shlex.split(line[len("ExecStart="):]))
        self.assertEqual(o["listen"], "127.0.0.1:18781")        # Tailscale Serve aynı yeri gösterir
        self.assertEqual(o["bridge-listen"], "100.114.36.9:8090")  # PC Edge aynı adrese yükler
        self.assertEqual(o["db"], "/home/m/orun-backend-bench.sqlite")
        self.assertEqual(o["registry"], "/home/m/orun-credentials.sqlite")
        self.assertNotIn("bench-test-key", o)

    def test_defaults_and_refusals(self):
        b = backend_config(["/usr/bin/python3", "x.py", "--token", "abcdefghij", "--bench-test-key"])
        self.assertEqual((b["port"], b["token"], b["bench"]), (8090, "abcdefghij", True))
        with self.assertRaises(Exception):
            shared_config(["python3", "x.py", "--state-dir", "/s"])   # site/port yok
        with self.assertRaises(Exception):
            exec_argv("{ path=/x ; ignore_errors=no }")


if __name__ == "__main__":
    unittest.main()
