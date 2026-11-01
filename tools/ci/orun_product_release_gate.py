#!/usr/bin/env python3
"""ORUN product cutover gate: inspect source and optional read-only Tracker snapshot.

No flashing, USB writes, data deletion, service restart or credential access.
This is intentionally a *blocking* product-release gate. Host/bench success
must not be mistaken for a normal-image end-to-end field release.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass(frozen=True)
class Check:
    key: str
    ok: bool
    detail: str


def default_env(text: str) -> str:
    """Only build flags of the normal [env:rak4630], not inherited bench flags."""
    match = re.search(r"(?ms)^\[env:rak4630\]\s*\n(.*?)(?=^\[|\Z)", text)
    if match is None:
        raise ValueError("normal rak4630 PlatformIO environment is missing")
    return match.group(1)


def macros(text: str) -> set[str]:
    return set(re.findall(r"(?m)^\s*-D\s*([A-Za-z_][A-Za-z_0-9]*)\b", text))


def parse_line(snapshot: str, prefix: str) -> dict[str, str]:
    for line in snapshot.splitlines():
        stripped = line.strip()
        if stripped.startswith(prefix + " "):
            return dict(re.findall(r"([A-Za-z_][A-Za-z_0-9]*)=([^\s]+)", stripped))
    return {}


def check_sources(root: Path) -> list[Check]:
    ini = (root / "firmware" / "platformio.ini").read_text(encoding="utf-8")
    source = (root / "firmware" / "src" / "main.cpp").read_text(encoding="utf-8")
    env_flags = macros(default_env(ini))
    public_keys = {"ORUN_SF5L0_BENCH_TEST_KEYS", "ORUN_SF3_RUNTIME_QUAL"}
    # Pilot is intentionally insufficient: one TX per boot without field retry.
    tracker_ready = "ORUN_PRODUCT_TRACKER_DURABLE_TX" in env_flags
    gateway_ready = "ORUN_PRODUCT_GATEWAY_DURABLE_CUSTODY" in env_flags
    commissioning_ready = "ORUN_PRODUCT_SECURE_COMMISSIONING" in env_flags
    # A successful source scan cannot prove field RF, flash or the app.
    return [
        Check("normal-image-found", True, "PlatformIO [env:rak4630] mevcut"),
        Check("no-public-test-key", not bool(env_flags & public_keys),
              "Normal firmware kamuya açık test anahtarlarını etkinleştirmemeli"),
        Check("tracker-secure-tx", tracker_ready and
              "TrackerProductTxPilot" in source,
              "Normal firmware'de sürekli güvenli TX/yeniden deneme henüz onaylanmadı"
              if not tracker_ready else "Ürün TX bayrağı bulundu; fiziksel test ayrıca gerekli"),
        Check("gateway-durable-custody", gateway_ready and
              "GatewayProductCustodyRuntime" in source,
              "Gateway kalıcı custody + yetkili ACK yalnız bench image'de"
              if not gateway_ready else "Gateway üretim bayrağı bulundu; fiziksel test ayrıca gerekli"),
        Check("secure-commissioning", commissioning_ready,
              "Yeni cihaz için gerçek credential/grant kurulumu normal image'de yok"
              if not commissioning_ready else "Ürün provisioning bayrağı bulundu; gerçek cihaz kontrolü gerekli"),
    ]


def check_physical_evidence(evidence_path: Path | None) -> list[Check]:
    """Human-reviewable evidence manifest; never auto-assert hardware PASS.

    Exact observed record identity must match at TX, Gateway durable commit,
    Tracker release, Edge, Backend and Android. All proof links are required.
    The release reviewer must also inspect the referenced logs; booleans alone
    are not evidence.
    """
    if evidence_path is None:
        return [Check("physical-e2e-evidence", False,
                      "Gerçek iki cihazlı ve Android uçtan uca kanıt manifesti yok")]
    obj = json.loads(evidence_path.read_text(encoding="utf-8"))
    if not isinstance(obj, dict):
        return [Check("physical-e2e-evidence", False, "Kanıt JSON nesnesi değil")]
    identity = obj.get("record_identity")
    steps = obj.get("steps")
    faults = obj.get("fault_tests")
    required = ("tracker_tx", "gateway_durable", "gateway_ack",
                "tracker_release", "edge_sqlite", "backend_verified",
                "android_map")
    required_faults = ("ack_lost_retry", "duplicate", "tracker_reboot",
                       "gateway_reboot", "offline_edge", "power_cut")
    valid_ids = all(re.fullmatch(r"[0-9A-Fa-f]{16}", str(obj.get(k, "")))
                    for k in ("tracker_id", "gateway_id"))
    valid_firmware = bool(re.fullmatch(r"[0-9a-f]{64}",
                                      str(obj.get("firmware_sha256", ""))))
    same_record = isinstance(identity, str) and bool(
        re.fullmatch(r"[0-9A-Fa-f]{16}:[1-9][0-9]*", identity))
    valid_steps = isinstance(steps, dict) and all(
        isinstance(steps.get(k), dict) and
        steps[k].get("record_identity") == identity and
        isinstance(steps[k].get("log_sha256"), str) and
        bool(re.fullmatch(r"[0-9a-f]{64}", steps[k]["log_sha256"]))
        for k in required)
    valid_faults = isinstance(faults, dict) and all(
        isinstance(faults.get(k), str) and
        bool(re.fullmatch(r"[0-9a-f]{64}", faults[k]))
        for k in required_faults)
    ok = valid_ids and valid_firmware and same_record and valid_steps and valid_faults and obj.get("public_test_keys") is False
    return [Check("physical-e2e-evidence", bool(ok),
                  "Kimlik/sürüm/eşleşen kayıt/7 halkalı log kanıtı/6 hata testi "
                  + ("manifestte mevcut; loglar insan tarafından incelenmeli" if ok
                     else "eksik veya geçersiz"))]


def check_tracker_snapshot(snapshot: str) -> list[Check]:
    role = parse_line(snapshot, "ROLE")
    tracking = parse_line(snapshot, "APP TRACKING")
    product = parse_line(snapshot, "PRODUCT STORE")
    # PRODUCT STORE has two distinct lines; first line is authoritative for mount.
    storage = parse_line(snapshot, "APP STORAGE")
    security = parse_line(snapshot, "SF3 QUAL")
    checks = [
        Check("tracker-role", role.get("mode") in ("AUTO", "SERVICES", "OVERRIDE")
              and "ROLE TRACKER " in snapshot,
              "Tracker rolü kesin doğrulanamadı" if "ROLE TRACKER " not in snapshot else "Tracker rolü doğrulandı"),
        Check("gnss-real-fixes", int(tracking.get("fresh", "0")) > 0,
              "GNSS fresh=" + tracking.get("fresh", "bilinmiyor")),
        Check("product-region-mounted", product.get("mounted") == "yes" and
              product.get("decision") == "MOUNT",
              "Ürün kayıt alanı: decision=" + product.get("decision", "?") +
              ", mounted=" + product.get("mounted", "?") +
              ", unknown=" + product.get("unknown", "?")),
        Check("history-ready", storage.get("history_ready") == "yes",
              "HistoryStore: history_ready=" + storage.get("history_ready", "ölçülmedi")),
        Check("security-provisioned", security.get("security") == "PROVISIONED",
              "SF3 QUAL güvenlik durumu: " + security.get("security", "doğrulanmadı") +
              " (normal firmware'de SF3? komutu yoksa güvenlik hazır sayılmaz)"),
        Check("real-gateway-ack", False,
              "Snapshot Gateway'in yeni Tracker paketine ACK verdiğini ve Tracker'ın DURABLE release yazdığını göstermez"),
    ]
    return checks


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path,
                        default=Path(__file__).resolve().parents[2])
    parser.add_argument("--tracker-snapshot", type=Path,
                        help="Önceden alınmış salt-okunur seri konsol çıktısı; cihaza bağlanmaz")
    parser.add_argument("--evidence", type=Path,
                        help="Gerçek donanım + Backend + Android log SHA kanıt manifesti")
    parser.add_argument("--json", action="store_true", help="Makine-okunur sonuç")
    args = parser.parse_args(argv)

    try:
        checks = check_sources(args.root)
        checks += check_physical_evidence(args.evidence)
        if args.tracker_snapshot is not None:
            checks += check_tracker_snapshot(
                args.tracker_snapshot.read_text(encoding="utf-8"))
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print("ORUN PREFLIGHT ERROR:", exc, file=sys.stderr)
        return 3

    ready = all(c.ok for c in checks)
    if args.json:
        print(json.dumps({"release_ready": ready,
                          "checks": [asdict(c) for c in checks]},
                         ensure_ascii=False, indent=2))
    else:
        print("ORUN PRODUCT RELEASE: " + ("READY" if ready else "BLOCKED"))
        for check in checks:
            print(("PASS " if check.ok else "BLOCK") + " " +
                  check.key + " — " + check.detail)
        if not ready:
            print("Firmware yüklemeyin / kayıt silmeyin: eksikler giderilip "
                  "gerçek iki cihazlı kabul testi tamamlanmalı.")
    return 0 if ready else 2


if __name__ == "__main__":
    raise SystemExit(main())
