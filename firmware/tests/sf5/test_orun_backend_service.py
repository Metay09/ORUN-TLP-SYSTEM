#!/usr/bin/env python3
"""Loopback-only backend service smoke: private token, health and auth; no Gateway.

Run: python3 firmware/tests/sf5/test_orun_backend_service.py
Uses an isolated temporary database/registry; never touches the owner's
credentials, ledger, or service. No internet, USB, or firmware flashing.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import select
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[3]
BACKEND = ROOT / "backend" / "orun_backend_bench.py"
TOKEN = "orun-test-token-not-production"


class BackendServiceTest(unittest.TestCase):
    def start_args(self, folder: Path, token_file: Path) -> list[str]:
        return [
            sys.executable, "-u", str(BACKEND),
            "--token-file", str(token_file),
            "--db", str(folder / "ledger.sqlite"),
            "--registry", str(folder / "registry.sqlite"),
            "--host", "127.0.0.1", "--port", "0",
        ]

    def test_private_token_file_serves_health_and_authorized_keys(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            token = folder / "token"
            token.write_text(TOKEN + "\n")
            token.chmod(0o600)
            server = subprocess.Popen(
                self.start_args(folder, token), stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, bufsize=1,
            )
            try:
                self.assertIsNotNone(server.stdout)
                ready, _, _ = select.select([server.stdout], [], [], 10)
                self.assertTrue(ready, "backend did not emit its ready line")
                line = server.stdout.readline()
                self.assertNotIn(TOKEN, line, "secret must never be logged")
                url = re.search(r"http://127\.0\.0\.1:(\d+)/", line)
                self.assertIsNotNone(url, f"unexpected startup: {line!r}")
                origin = url.group(0)[:-1]
                with urlopen(origin + "/v1/health", timeout=5) as response:
                    self.assertEqual(json.load(response)["ok"], True)
                req = Request(origin + "/v1/edge/keys",
                              headers={"Authorization": "Bearer " + TOKEN})
                with urlopen(req, timeout=5) as response:
                    self.assertEqual(json.load(response), {"keys": []})
                with self.assertRaises(HTTPError) as unauthorized:
                    urlopen(origin + "/v1/edge/keys", timeout=5)
                self.assertEqual(unauthorized.exception.code, 401)
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)
                if server.stdout:
                    server.stdout.close()

    def test_world_readable_token_file_is_refused_before_binding(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            token = folder / "insecure-token"
            token.write_text(TOKEN + "\n")
            token.chmod(0o644)
            result = subprocess.run(
                self.start_args(folder, token), capture_output=True, text=True,
                timeout=10, check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("private regular file", result.stderr)
            self.assertFalse((folder / "ledger.sqlite").exists())
            self.assertFalse((folder / "registry.sqlite").exists())


if __name__ == "__main__":
    unittest.main()
