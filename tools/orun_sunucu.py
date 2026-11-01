#!/usr/bin/env python3
"""ORUN sunucusu (1a): tek süreç.

Bugüne kadar ayrı çalışan üç servisin işini tek programda yapar:
- doğrulanmış kayıt defteri ve yüklemeler (eski orun-backend),
- hesap girişi, ortak veri, telefon için anahtar ve kayıt okuma
  (eski orun-shared-sync + orun-observation-readonly).

İki dinleme adresi olabilir:
- hesap adresi (zorunlu): uygulamanın bağlandığı yer. HTTPS'i önündeki
  ters vekil sağlar (Tailscale Serve ya da Coolify).
- köprü adresi (isteğe bağlı): gateway köprüsü (PC Edge ya da ileride
  telefon/Linux köprüsü) buraya yükler. Yalnız POST /v1/edge/batch ve
  GET /v1/health; köprü anahtarı (token dosyası) ister.
"""
from __future__ import annotations

import argparse
import hmac
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import stat
import sys
import threading

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from backend.account_login import AccountStore  # noqa: E402
from backend.account_login_http import account_handler_factory  # noqa: E402
from backend.local_observations import LocalObservations  # noqa: E402
from backend.orun_backend_bench import (  # noqa: E402
    Backend, MAX_BODY, load_private_token_file, oc,
)
from tools.orun_shared_sync_loopback import StartupRefused, prepare  # noqa: E402


def _address(text: str) -> tuple[str, int]:
    host, sep, port = text.rpartition(":")
    if not sep or not host or not port.isdecimal() or not 0 <= int(port) <= 65535:
        raise argparse.ArgumentTypeError("HOST:PORT bekleniyor")
    return host, int(port)


def bridge_handler(local: LocalObservations, token: str):
    expected = token.encode()

    class Bridge(BaseHTTPRequestHandler):
        server_version = "ORUN"
        sys_version = ""

        def log_message(self, *_args):
            pass

        def _json(self, code: int, obj: dict) -> None:
            raw = json.dumps(obj, separators=(",", ":")).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(raw)

        def _authorized(self) -> bool:
            auth = self.headers.get("Authorization", "")
            given = auth[7:].encode() if auth.startswith("Bearer ") else b""
            return hmac.compare_digest(given, expected)

        def do_GET(self):  # noqa: N802
            if self.path != "/v1/health":
                return self._json(404, {"error": "not_found"})
            if not self._authorized():
                return self._json(401, {"error": "token"})
            return self._json(200, {"ok": True,
                                    "records": local.backend.ledger.record_count()})

        def do_POST(self):  # noqa: N802
            if self.path != "/v1/edge/batch":
                return self._json(404, {"error": "not_found"})
            if not self._authorized():
                return self._json(401, {"error": "token"})
            size = self.headers.get("Content-Length", "")
            if (self.headers.get("Transfer-Encoding") or not size.isdecimal()
                    or int(size) > MAX_BODY):
                return self._json(413, {"error": "size"})
            status, body = local.batch(self.rfile.read(int(size)))
            return self._json(status, body)

    return Bridge


def build(state_dir: str | Path, db: str | Path, registry: str | Path, site: str,
          *, bench_test_key: bool = False) -> tuple[LocalObservations, type]:
    store, enrollment = prepare(state_dir, reuse_state=True)
    account_db = Path(state_dir) / "owner_accounts.sqlite"
    if (account_db.is_symlink() or not account_db.is_file() or
            account_db.stat().st_uid != os.geteuid() or
            stat.S_IMODE(account_db.stat().st_mode) & 0o077):
        raise StartupRefused("EXPLICIT_PRIVATE_OWNER_ACCOUNT_BOOTSTRAP_REQUIRED")
    backend = Backend(Path(db), bench_test_key,
                      registry=oc.CredentialRegistry(str(registry)))
    local = LocalObservations(backend, site)
    handler = account_handler_factory(
        store, enrollment, AccountStore(account_db), observation_relay=local)
    return local, handler


def serve(account: tuple[str, int], handler: type,
          bridge: tuple[str, int] | None = None, bridge_cls: type | None = None) -> None:
    servers = [ThreadingHTTPServer(account, handler)]
    if bridge is not None and bridge_cls is not None:
        servers.append(ThreadingHTTPServer(bridge, bridge_cls))
    for s in servers:
        s.daemon_threads = True
    print("ORUN_SUNUCU_HESAP=%s:%d" % servers[0].server_address[:2], flush=True)
    if len(servers) > 1:
        print("ORUN_SUNUCU_KOPRU=%s:%d" % servers[1].server_address[:2], flush=True)
    threads = [threading.Thread(target=s.serve_forever, kwargs={"poll_interval": 0.2},
                                daemon=True) for s in servers]
    for t in threads:
        t.start()
    try:
        for t in threads:
            t.join()
    finally:
        for s in servers:
            s.shutdown()
            s.server_close()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="ORUN sunucusu: tek süreç")
    ap.add_argument("--state-dir", required=True,
                    help="hesap ve ortak veri klasörü (0700; owner_accounts.sqlite dahil)")
    ap.add_argument("--db", required=True, help="doğrulanmış kayıt defteri (SQLite)")
    ap.add_argument("--registry", required=True, help="tasma kimlik kayıtları (SQLite)")
    ap.add_argument("--site", required=True, help="hesap sitesi, ör. orun-main")
    ap.add_argument("--listen", type=_address, default=("127.0.0.1", 18781),
                    help="uygulama/hesap adresi HOST:PORT (varsayılan 127.0.0.1:18781)")
    ap.add_argument("--bridge-listen", type=_address,
                    help="gateway köprüsü adresi HOST:PORT (ör. 100.x.y.z:8090)")
    ap.add_argument("--bridge-token-file",
                    help="köprü anahtarı dosyası (0600); --bridge-listen ile zorunlu")
    ap.add_argument("--bench-test-key", action="store_true",
                    help="eski masa testi kayıtlarının herkese açık test anahtarını kabul et")
    args = ap.parse_args(argv)
    if bool(args.bridge_listen) != bool(args.bridge_token_file):
        ap.error("--bridge-listen ve --bridge-token-file birlikte verilmeli")
    try:
        local, handler = build(args.state_dir, args.db, args.registry, args.site,
                               bench_test_key=args.bench_test_key)
        bridge_cls = None
        if args.bridge_token_file:
            bridge_cls = bridge_handler(local, load_private_token_file(args.bridge_token_file))
    except (StartupRefused, OSError, ValueError) as exc:
        print(f"ORUN_SUNUCU_BASLAMADI={exc}", file=sys.stderr, flush=True)
        return 2
    serve(args.listen, handler, args.bridge_listen, bridge_cls)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
