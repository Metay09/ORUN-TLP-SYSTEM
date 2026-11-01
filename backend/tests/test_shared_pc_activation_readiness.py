"""S4E activation HTTP readiness regression; no systemd or ORUN live files."""
from __future__ import annotations

import http.server
from pathlib import Path
import socket
import tempfile
import threading
import time
import unittest

from tools.ci.activate_shared_pc_runtime import ActivationRefused, identity


class IdentityHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if (self.path == "/api/v1/sync/identity" and
                self.headers.get("Authorization") == "Bearer unit-test-token"):
            payload = b'{"site":"site-a","installation":"pc-a"}'
            self.send_response(200)
        else:
            payload = b'{"error":"unauthorized"}'
            self.send_response(401)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


class SidecarActivationReadinessTests(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory(prefix="orun-sidecar-readiness-")
        self.addCleanup(work.cleanup)
        self.token = Path(work.name) / "dummy.token"
        self.token.write_text("unit-test-token\n", encoding="ascii")

    def _port(self):
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            return sock.getsockname()[1]

    def test_late_bind_retry_reaches_authenticated_identity(self):
        port = self._port()
        ready = threading.Event()
        closed = threading.Event()

        def serve():
            time.sleep(0.35)
            with http.server.HTTPServer(("127.0.0.1", port), IdentityHandler) as srv:
                ready.set()
                while not closed.is_set():
                    srv.handle_request()
        worker = threading.Thread(target=serve, daemon=True)
        worker.start()
        try:
            identity(port, self.token, "site-a", "pc-a", wait_seconds=3)
            self.assertTrue(ready.is_set())
        finally:
            closed.set()
            # Wake handler after stop flag (HTTPServer.handle_request blocks).
            try:
                socket.create_connection(("127.0.0.1", port), 0.2).close()
            except OSError:
                pass
            worker.join(timeout=3)

    def test_http_401_denied_immediately_not_retried_as_urLError(self):
        server = http.server.HTTPServer(("127.0.0.1", 0), IdentityHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            self.token.write_text("invalid-token\n", encoding="ascii")
            with self.assertRaisesRegex(ActivationRefused, "SHARED_BACKEND_HTTP_DENIED_401"):
                identity(server.server_port, self.token, "site-a", "pc-a", wait_seconds=3)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)

    def test_unavailable_port_returns_safe_specific_code(self):
        with self.assertRaisesRegex(ActivationRefused, "SHARED_BACKEND_HTTP_NOT_READY"):
            identity(self._port(), self.token, "site-a", "pc-a", wait_seconds=0.35)


if __name__ == "__main__":
    unittest.main(verbosity=2)
