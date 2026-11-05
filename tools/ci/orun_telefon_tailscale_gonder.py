#!/usr/bin/env python3
"""Send an ISOLATED ORUN 4c USB read-only test APK to one Tailscale Android.

Usage (after tools/ci/orun_telefon_usb_dogrula.sh passes):
  python3 tools/ci/orun_telefon_tailscale_gonder.py

This script never installs an APK, accesses USB, or modifies any device.
It refuses wrong Android application IDs and ambiguous target selection.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APK = ROOT / "edge/app/android/app/build/outputs/apk/debug/app-debug.apk"
EXPECTED = "tr.orun.edge.usbtest"


def fail(message: str) -> None:
    raise SystemExit("DUR: " + message)


def checked(args: list[str]) -> str:
    try:
        result = subprocess.run(args, check=True, capture_output=True, text=True)
        return result.stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        detail = (getattr(exc, "stderr", None) or str(exc)).strip()
        fail("Komut başarısız: " + " ".join(args[:3]) + " — " + detail[:280])


def sdk_candidates() -> list[Path]:
    roots: list[Path] = []
    local_props = ROOT / "edge/app/android/local.properties"
    if local_props.is_file():
        for line in local_props.read_text(encoding="utf-8").splitlines():
            if line.startswith("sdk.dir="):
                roots.append(Path(line.split("=", 1)[1].replace("\\ ", " ")))
    for name in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        if os.environ.get(name):
            roots.append(Path(os.environ[name]))
    roots.append(Path.home() / "Android/Sdk")
    options: list[Path] = []
    detected = shutil.which("aapt")
    if detected:
        options.append(Path(detected))
    for root in roots:
        if root.exists():
            options.extend(sorted(root.glob("build-tools/*/aapt"), reverse=True))
    return [p for p in options if p.is_file() and os.access(p, os.X_OK)]


def verify_android_id() -> None:
    for aapt in sdk_candidates():
        try:
            out = subprocess.run(
                [str(aapt), "dump", "badging", str(APK)],
                capture_output=True, text=True, check=True,
            ).stdout
        except (OSError, subprocess.CalledProcessError):
            continue
        m = re.search(r"^package: name='([^']+)'", out, re.M)
        if not m:
            continue
        actual = m.group(1)
        if actual != EXPECTED:
            fail(f"APK paket kimliği {actual!r}, beklenen {EXPECTED!r}. Gönderim iptal.")
        print("APK uygulama kimliği doğrulandı:", actual)
        return
    fail("Android SDK aapt ile APK kimliği okunamadı; güvenli gönderim durduruldu.")


def target_android() -> tuple[str, str]:
    status = json.loads(checked(["tailscale", "status", "--json"]))
    peers = list(status.get("Peer", {}).values())
    androids = [p for p in peers if (p.get("OS") or "").lower() == "android"]
    online = [p for p in androids if p.get("Online") is True]
    if len(online) != 1:
        print("Tailscale'deki Android cihazlar:", file=sys.stderr)
        for p in androids:
            print(" -", p.get("HostName") or p.get("DNSName") or "(isimsiz)",
                  "| çevrimiçi=" + str(p.get("Online")),
                  "| IP=" + ",".join(p.get("TailscaleIPs") or []),
                  file=sys.stderr)
        fail("Tek çevrimiçi Android hedefi yok. Telefonda Tailscale'i aç ve tekrar dene.")
    p = online[0]
    ipv4 = [ip for ip in (p.get("TailscaleIPs") or []) if re.fullmatch(r"100\.\d+\.\d+\.\d+", ip)]
    if len(ipv4) != 1:
        fail("Hedef telefon için tek bir Tailscale IPv4 adresi belirlenemedi.")
    return p.get("HostName") or p.get("DNSName") or "Android", ipv4[0]


def main() -> None:
    if not APK.is_file() or APK.stat().st_size < 4096:
        fail("Derlenmiş debug APK bulunamadı. Önce USB doğrulama/derleme komutunu çalıştır.")
    verify_android_id()
    digest = hashlib.sha256(APK.read_bytes()).hexdigest()
    destination = Path.home() / "Downloads" / f"ORUN-USB-Test-{digest[:12]}.apk"
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if hashlib.sha256(destination.read_bytes()).hexdigest() != digest:
            fail("Aynı isimli farklı APK var; hiçbir dosyanın üzerine yazılmadı.")
    else:
        shutil.copyfile(APK, destination)
    print("APK:", destination)
    print("SHA256:", digest)
    name, ip = target_android()
    print(f"Telefon: {name} ({ip})")
    print("Tailscale ile APK gönderiliyor; kurulum yapılmaz.")
    subprocess.run(["tailscale", "file", "cp", str(destination), ip + ":"], check=True)
    print("TAILSCALE GÖNDERİM PASS. Android bildirimlerinden dosyayı aç.")
    print("Bu APK mevcut ORUN Edge'in yerine geçmez; ORUN USB Test ayrı uygulamadır.")
    print("Firmware yükleme kapalıdır. Önce yalnız USB kimliği/durum bilgisi oku.")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        fail("Tailscale aktarımı başarısız: " + str(exc))
