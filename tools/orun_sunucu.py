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
  telefon/Linux köprüsü) buraya yükler. Yalnız POST /v1/edge/batch,
  GET /v1/health ve GET /v1/bridge/ogk1 (gateway'in güncel imzalı izin
  paketi); köprü anahtarı (token dosyası) ister.

Köprü ayrıca bağlı olduğu gateway'in durumunu bildirir: POST /v1/bridge/gateway.
Hesap adresi çiftliğin gateway listesini verir: GET /api/v1/devices/gateways;
cihaz adları: GET /api/v1/devices/names, POST /api/v1/devices/name (2d);
tasmadaki alanlar: GET /api/v1/devices/fences, POST /api/v1/devices/fence (3a);
cihaz ayarları: GET /api/v1/devices/settings, POST /api/v1/devices/settings (istenen),
POST /api/v1/devices/settings/reported (cihazdan okunan) (4b);
işlemler (alan/ayar gönderme kaydı): GET /api/v1/devices/ops, POST /api/v1/devices/ops,
POST /api/v1/devices/ops/result;
telefon köprü olduğunda POST /api/v1/devices/gateway ile bildirir (2c).

Hesap adresi ayrıca internetsiz harita paketlerini verir (2. adım):
GET /api/v1/maps/catalog ve GET /api/v1/maps/file/<ad>.pmtiles; hesap
oturumu ister. Paketleri `tools/orun_harita.sh` hazırlar.
"""
from __future__ import annotations

import argparse
import hmac
import json
import os
import re
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
from backend.device_names import BadName, DeviceNames  # noqa: E402
from backend.device_settings import BadSettings, DeviceSettings  # noqa: E402
from backend.device_ops import BadOp, DeviceOps  # noqa: E402
from backend.gateway_status import BadStatus, GatewayStatus, parse_report  # noqa: E402
from backend.local_observations import LocalObservations  # noqa: E402
from backend.tracker_fences import BadFence, TrackerFences  # noqa: E402
from backend.map_packs import CATALOG_ROUTE, FILE_ROUTE, MapPacks, default_dir  # noqa: E402
from backend.orun_backend_bench import (  # noqa: E402
    Backend, MAX_BODY, load_private_token_file, oc,
)
from tools.orun_shared_sync_loopback import StartupRefused, prepare  # noqa: E402


def _address(text: str) -> tuple[str, int]:
    host, sep, port = text.rpartition(":")
    if not sep or not host or not port.isdecimal() or not 0 <= int(port) <= 65535:
        raise argparse.ArgumentTypeError("HOST:PORT bekleniyor")
    return host, int(port)


OGK1_QUERY = re.compile(
    r"/v1/bridge/ogk1\?gateway=([0-9A-F]{16})&active=(0|[1-9][0-9]{0,18})&issuer=([0-9A-F]{8})")


def farm_gateway_ids(local: LocalObservations) -> list[str]:
    registry = local.backend.registry
    if registry is None:
        return []
    try:
        return ["%016X" % g for g in registry.farm_gateways()]
    except oc.RegistryError:
        return []


def bridge_handler(local: LocalObservations, token: str):
    expected = token.encode()
    gateways = GatewayStatus(local.backend.db_path)

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
            match = OGK1_QUERY.fullmatch(self.path)
            if self.path != "/v1/health" and match is None:
                return self._json(404, {"error": "not_found"})
            if not self._authorized():
                return self._json(401, {"error": "token"})
            if match is not None:
                return self._ogk1(int(match[1], 16), int(match[2]), match[3])
            return self._json(200, {"ok": True,
                                    "records": local.backend.ledger.record_count()})

        def _ogk1(self, gateway: int, active: int, issuer_prefix: str) -> None:
            """C1: the signed grant snapshot this bridge should install now."""
            registry = local.backend.registry
            if registry is None:
                return self._json(503, {"error": "no_registry"})
            try:
                issuer = registry.issuer()
                if issuer is None:
                    return self._json(503, {"error": "no_issuer"})
                if oc.issuer_public(issuer)[:4].hex().upper() != issuer_prefix:
                    return self._json(409, {"error": "issuer_mismatch"})
                registry.note_gateway(gateway)       # köprüye bağlı gateway = çiftlikte
                pending = registry.pending_snapshot(gateway, active)
            except oc.RegistryError as exc:
                return self._json(409, {"error": "registry", "detail": str(exc)[:120]})
            if pending is None:
                return self._json(200, {"generation": active, "lines": []})
            generation, image = pending
            return self._json(200, {"generation": generation,
                                    "lines": oc.ogk1_upload_lines(image)})

        def do_POST(self):  # noqa: N802
            if self.path not in ("/v1/edge/batch", "/v1/bridge/gateway"):
                return self._json(404, {"error": "not_found"})
            if not self._authorized():
                return self._json(401, {"error": "token"})
            if self.path == "/v1/bridge/gateway":
                return self._gateway()
            size = self.headers.get("Content-Length", "")
            if (self.headers.get("Transfer-Encoding") or not size.isdecimal()
                    or int(size) > MAX_BODY):
                return self._json(413, {"error": "size"})
            status, body = local.batch(self.rfile.read(int(size)))
            return self._json(status, body)

        def _gateway(self) -> None:
            """2c: köprünün bağlı olduğu gateway: kimlik, USB/BLE, bellek."""
            size = self.headers.get("Content-Length", "")
            if self.headers.get("Transfer-Encoding") or not size.isdecimal() or not 1 <= int(size) <= 512:
                return self._json(400, {"error": "invalid_request"})
            try:
                gw, via, held, cap = parse_report(json.loads(self.rfile.read(int(size))), "pc")
                gateways.report(gw, via, held, cap)
                registry = local.backend.registry
                if registry is not None:
                    registry.note_gateway(int(gw, 16))
            except (BadStatus, ValueError, UnicodeDecodeError, oc.RegistryError):
                return self._json(400, {"error": "invalid_request"})
            return self._json(200, {"ok": True})

    return Bridge


DEVICE_ROUTES = ("/api/v1/devices/tracker/prepare", "/api/v1/devices/tracker/confirm")


def with_device_setup(handler: type, accounts: AccountStore, enrollment, local: LocalObservations) -> type:
    """1d/1e: hesabın sahibi uygulamadan tasma ekler (zone:write yetkisi)."""
    from backend.device_setup import SetupRefused, confirm_tracker, prepare_tracker

    class Handler(handler):
        def do_POST(self):  # noqa: N802
            if self.path not in DEVICE_ROUTES:
                return super().do_POST()
            bearer = self._bearer(self.headers)
            actor = accounts.resolve_session(enrollment, bearer) if bearer else None
            if actor is None or "zone:write" not in actor.scopes:
                return self._send(401, {"error": "unauthorized"})
            if actor.site_id != local.observation_site:
                return self._send(403, {"error": "site_not_authorized"})
            size = self.headers.get("Content-Length", "")
            if (self.headers.get("Transfer-Encoding") or not size.isdecimal() or
                    not 1 <= int(size) <= 4096 or
                    self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
                    != "application/json"):
                return self._send(400, {"error": "invalid_request"})
            registry = local.backend.registry
            if registry is None:
                return self._send(503, {"error": "no_registry"})
            try:
                body = json.loads(self.rfile.read(int(size)).decode("utf-8"))
                work = prepare_tracker if self.path.endswith("/prepare") else confirm_tracker
                return self._send(200, work(registry, body))
            except SetupRefused as exc:
                return self._send(409, {"error": exc.code, "message": str(exc)})
            except (ValueError, UnicodeDecodeError, oc.RegistryError):
                return self._send(400, {"error": "invalid_request"})

    return Handler


CHUNK = 1024 * 1024


def with_gateways(handler: type, accounts: AccountStore, enrollment, local: LocalObservations) -> type:
    """2c: çiftliğin gateway listesi; telefon köprü olduğunda durum bildirimi.
    2d: cihaz adları (hesapta)."""
    gateways = GatewayStatus(local.backend.db_path)
    names = DeviceNames(local.backend.db_path)
    fences = TrackerFences(local.backend.db_path)
    settings = DeviceSettings(local.backend.db_path)
    ops = DeviceOps(local.backend.db_path)

    class Handler(handler):
        def _farm_actor(self, scope: str):
            bearer = self._bearer(self.headers)
            actor = accounts.resolve_session(enrollment, bearer) if bearer else None
            if actor is None or scope not in actor.scopes:
                self._send(401, {"error": "unauthorized"})
                return None
            if actor.site_id != local.observation_site:
                self._send(403, {"error": "site_not_authorized"})
                return None
            return actor

        def do_GET(self):  # noqa: N802
            if self.path not in ("/api/v1/devices/gateways", "/api/v1/devices/names",
                                 "/api/v1/devices/fences", "/api/v1/devices/settings",
                                 "/api/v1/devices/ops"):
                return super().do_GET()
            if self._farm_actor("zone:read") is None:
                return None
            if self.path == "/api/v1/devices/names":
                return self._send(200, {"names": names.all()})
            if self.path == "/api/v1/devices/fences":
                return self._send(200, {"fences": fences.all()})
            if self.path == "/api/v1/devices/settings":
                return self._send(200, {"settings": settings.all()})
            if self.path == "/api/v1/devices/ops":
                return self._send(200, {"ops": ops.recent()})
            return self._send(200, {"gateways": gateways.list(farm_gateway_ids(local))})

        def do_POST(self):  # noqa: N802
            if self.path not in ("/api/v1/devices/gateway", "/api/v1/devices/name",
                                 "/api/v1/devices/fence", "/api/v1/devices/settings",
                                 "/api/v1/devices/settings/reported", "/api/v1/devices/ops",
                                 "/api/v1/devices/ops/result"):
                return super().do_POST()
            if self._farm_actor("zone:write") is None:
                return None
            size = self.headers.get("Content-Length", "")
            limit = (262144 if self.path == "/api/v1/devices/ops" else
                     16384 if self.path in ("/api/v1/devices/fence", "/api/v1/devices/settings") else 512)
            if (self.headers.get("Transfer-Encoding") or not size.isdecimal() or
                    not 1 <= int(size) <= limit):
                return self._send(400, {"error": "invalid_request"})
            if self.path == "/api/v1/devices/fence":
                try:
                    return self._send(200, fences.record(json.loads(self.rfile.read(int(size)))))
                except (BadFence, ValueError, UnicodeDecodeError):
                    return self._send(400, {"error": "invalid_fence"})
            if self.path in ("/api/v1/devices/ops", "/api/v1/devices/ops/result"):
                try:
                    body = json.loads(self.rfile.read(int(size)))
                    if self.path == "/api/v1/devices/ops":
                        return self._send(200, ops.create(body))
                    return self._send(200, ops.result(body))
                except (BadOp, ValueError, UnicodeDecodeError):
                    return self._send(400, {"error": "invalid_op"})
            if self.path in ("/api/v1/devices/settings", "/api/v1/devices/settings/reported"):
                try:
                    body = json.loads(self.rfile.read(int(size)))
                    if self.path == "/api/v1/devices/settings":
                        return self._send(200, settings.want(body))
                    return self._send(200, settings.report(body))
                except (BadSettings, ValueError, UnicodeDecodeError):
                    return self._send(400, {"error": "invalid_settings"})
            if self.path == "/api/v1/devices/name":
                try:
                    body = json.loads(self.rfile.read(int(size)))
                    dev, text = names.set(body.get("device") if isinstance(body, dict) else None,
                                          body.get("name") if isinstance(body, dict) else None)
                except (BadName, ValueError, UnicodeDecodeError):
                    return self._send(400, {"error": "invalid_name"})
                return self._send(200, {"device": dev, "name": text})
            try:
                gw, via, held, cap = parse_report(json.loads(self.rfile.read(int(size))), "phone")
                gateways.report(gw, via, held, cap)
                registry = local.backend.registry
                if registry is not None:
                    registry.note_gateway(int(gw, 16))
            except (BadStatus, ValueError, UnicodeDecodeError, oc.RegistryError):
                return self._send(400, {"error": "invalid_request"})
            return self._send(200, {"ok": True})

    return Handler


def with_maps(handler: type, accounts: AccountStore, enrollment, maps: MapPacks) -> type:
    """2: internetsiz harita paketleri; hesap oturumu (zone:read) ister."""

    class Handler(handler):
        def do_GET(self):  # noqa: N802
            path = self.path
            if path != CATALOG_ROUTE and not path.startswith(FILE_ROUTE):
                return super().do_GET()
            bearer = self._bearer(self.headers)
            actor = accounts.resolve_session(enrollment, bearer) if bearer else None
            if actor is None or "zone:read" not in actor.scopes:
                return self._send(401, {"error": "unauthorized"})
            if path == CATALOG_ROUTE:
                return self._send(200, maps.catalog())
            target = maps.file(path[len(FILE_ROUTE):])
            if target is None:
                return self._send(404, {"error": "not_found"})
            try:
                handle = target.open("rb")
            except OSError:
                return self._send(404, {"error": "not_found"})
            with handle:
                size = os.fstat(handle.fileno()).st_size
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(size))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                try:
                    while block := handle.read(CHUNK):
                        self.wfile.write(block)
                except (BrokenPipeError, ConnectionResetError):
                    self.close_connection = True

    return Handler


def build(state_dir: str | Path, db: str | Path, registry: str | Path, site: str,
          *, bench_test_key: bool = False,
          maps_dir: str | Path | None = None) -> tuple[LocalObservations, type]:
    store, enrollment = prepare(state_dir, reuse_state=True)
    account_db = Path(state_dir) / "owner_accounts.sqlite"
    if (account_db.is_symlink() or not account_db.is_file() or
            account_db.stat().st_uid != os.geteuid() or
            stat.S_IMODE(account_db.stat().st_mode) & 0o077):
        raise StartupRefused("EXPLICIT_PRIVATE_OWNER_ACCOUNT_BOOTSTRAP_REQUIRED")
    backend = Backend(Path(db), bench_test_key,
                      registry=oc.CredentialRegistry(str(registry)))
    local = LocalObservations(backend, site)
    accounts = AccountStore(account_db)
    handler = account_handler_factory(
        store, enrollment, accounts, observation_relay=local)
    handler = with_device_setup(handler, accounts, enrollment, local)
    handler = with_gateways(handler, accounts, enrollment, local)
    maps = MapPacks(maps_dir if maps_dir is not None else default_dir())
    return local, with_maps(handler, accounts, enrollment, maps)


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
    ap.add_argument("--maps-dir",
                    help="internetsiz harita paketleri (varsayılan ~/.local/share/orun/maps)")
    ap.add_argument("--bench-test-key", action="store_true",
                    help="eski masa testi kayıtlarının herkese açık test anahtarını kabul et")
    args = ap.parse_args(argv)
    if bool(args.bridge_listen) != bool(args.bridge_token_file):
        ap.error("--bridge-listen ve --bridge-token-file birlikte verilmeli")
    try:
        local, handler = build(args.state_dir, args.db, args.registry, args.site,
                               bench_test_key=args.bench_test_key,
                               maps_dir=args.maps_dir)
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
