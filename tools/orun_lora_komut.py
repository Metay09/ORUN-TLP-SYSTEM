#!/usr/bin/env python3
"""6a/6b masa denemesi: sunucunun imzalayacağı LoRa ayar komutu.

Komutu araç, sunucunun tasma anahtarıyla imzalar (6c'de bunu sunucu yapacak).

USB'den (6a): tasma USB'ye takılı; komut tasmaya LoRa'dan gelmiş gibi aynı
servise gider.
  python3 tools/orun_lora_komut.py --tasma <by-id yolu> --aralik 300

Telsizden (6b): komut gateway'e verilir; gateway tasmanın bir sonraki
raporunu duyunca telsizden gönderir, tasmanın imzalı cevabını geri iletir.
PC Edge deneme boyunca durdurulur (gateway'in USB'sini o tutuyor), sonunda
yeniden başlar. Tasmanın durumu --tasma verildiyse USB'den, yoksa telsizden
öğrenilir (o zaman iki rapor beklenir).
  python3 tools/orun_lora_komut.py --gateway <by-id> --kimlik 52E16773141A8887 --aralik 300
  python3 tools/orun_lora_komut.py --gateway <by-id> --tasma <by-id> --aralik 180

Önce bilerek eski durumla bir komut gider: tasma "STALE" der ve imzalı
cevabında güncel durumunu söyler; araç komutu o duruma göre yeniden imzalar.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import secrets
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from backend import lora_command as lc  # noqa: E402
from backend import orun_credentials as oc  # noqa: E402

PC_EDGE = "orun-pc-edge.service"


def port_id(port: str) -> str:
    return Path(port).name.split("_Board_")[-1].split("-if")[0].upper()


def usb_exchange(ser, frame: bytes, key: bytes, timeout: float = 12.0) -> lc.Result:
    ser.reset_input_buffer()
    ser.write(b"APP LORA CMD " + frame.hex().upper().encode() + b"\n")
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = ser.readline().decode(errors="replace").strip()
        if line.startswith("APP LORA via=") and "PENDING" not in line:
            raise SystemExit(f"Tasma komutu almadı: {line}")
        if line.startswith("APP LORA RESULT ") and "hex=" in line:
            return lc.parse_result(key, bytes.fromhex(line.rsplit("hex=", 1)[-1]))
    raise SystemExit("Tasmadan cevap gelmedi.")


def lora_exchange(gw, frame: bytes, key: bytes, device: int, timeout: float) -> lc.Result:
    gw.reset_input_buffer()
    gw.write(b"APP LORA QUEUE " + frame.hex().upper().encode() + b"\n")
    deadline = time.time() + timeout
    queued = False
    while time.time() < deadline:
        line = gw.readline().decode(errors="replace").strip()
        if not line:
            continue
        if line.startswith("LORA QUEUED"):
            if "ADDED" not in line and "REPLACED" not in line:
                raise SystemExit(f"Gateway komutu almadı: {line}")
            queued = True
            print(f"  gateway'de, tasmanın raporu bekleniyor ({int(deadline - time.time())} sn içinde)…")
        elif line.startswith("LORA SENT"):
            print(f"  telsizden gönderildi: {line.split('attempt=')[-1]}. deneme")
        elif line.startswith("LORA DROPPED"):
            raise SystemExit("Gateway komutu bıraktı: tasma 6 raporda cevap vermedi.")
        elif line.startswith("LORA RESULT") and "hex=" in line:
            try:
                r = lc.parse_result(key, bytes.fromhex(line.rsplit("hex=", 1)[-1]))
            except (lc.BadResult, ValueError):
                continue  # başka bir tasmanın ya da bozuk cevap
            if r.device == device:
                return r
        elif line.startswith("LORA QUEUE refused"):
            raise SystemExit("Bu port bir gateway değil.")
    raise SystemExit("Süre doldu: " + ("tasmadan cevap gelmedi." if queued else "gateway cevap vermedi."))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tasma", "--port", dest="tasma", help="tasmanın USB by-id yolu")
    ap.add_argument("--gateway", help="gateway'in USB by-id yolu (telsizden dene)")
    ap.add_argument("--kimlik", help="tasma kimliği (16 hane), --tasma yoksa")
    ap.add_argument("--registry", default=str(Path.home() / "orun-credentials.sqlite"))
    ap.add_argument("--aralik", type=int, help="rapor aralığı (sn)")
    ap.add_argument("--ble", type=int, help="rapordan sonra Bluetooth (sn)")
    ap.add_argument("--bekle", type=int, default=900, help="telsizde cevap için en çok (sn)")
    a = ap.parse_args()
    entries = ([(1, a.aralik)] if a.aralik is not None else []) + ([(3, a.ble)] if a.ble is not None else [])
    if not entries:
        ap.error("--aralik ya da --ble ver")
    if not a.tasma and not (a.gateway and a.kimlik):
        ap.error("--tasma ya da (--gateway ile --kimlik) ver")
    import serial  # pyserial

    device_hex = port_id(a.tasma) if a.tasma else a.kimlik.upper()
    cred = oc.CredentialRegistry(a.registry).active(int(device_hex, 16)) if len(device_hex) == 16 else None
    if cred is None:
        raise SystemExit(f"Bu tasmanın anahtarı kayıtlarda yok ({device_hex}).")
    key = oc.ble_write_key(cred)

    def command(inc: int, rev: int) -> bytes:
        return lc.build_command(key, cred.device_id, cred.key_epoch, secrets.randbits(32), inc, rev, entries)

    # 1) Tasmanın güncel durumu (bilerek eski durumla: STALE cevabı onu taşır).
    tasma = serial.Serial(a.tasma, 115200, timeout=0.5, write_timeout=2, exclusive=True) if a.tasma else None
    stopped_edge = False
    try:
        gw = None
        if a.gateway:
            stopped_edge = subprocess.call(["systemctl", "--user", "is-active", "--quiet", PC_EDGE]) == 0
            if stopped_edge:
                print("PC Edge deneme boyunca durduruluyor…")
                subprocess.call(["systemctl", "--user", "stop", PC_EDGE])
                time.sleep(1)
            gw = serial.Serial(a.gateway, 115200, timeout=0.5, write_timeout=2, exclusive=True)
        if tasma is not None:
            now = usb_exchange(tasma, command(0, 0), key)
        else:
            print("Tasmanın durumu telsizden soruluyor…")
            now = lora_exchange(gw, command(0, 0), key, cred.device_id, a.bekle)
        if now.outcome != "STALE":
            raise SystemExit(f"Beklenmeyen cevap: {now}")
        print(f"Tasmanın durumu: sürüm {now.revision}, aralık {now.interval_s} sn, Bluetooth {now.ble_window_s} sn")

        # 2) Gerçek komut, o duruma bağlı.
        cmd = command(now.incarnation, now.revision)
        if gw is not None:
            print("Komut gateway'e veriliyor; telsizden gidecek…")
            done = lora_exchange(gw, cmd, key, cred.device_id, a.bekle)
            print(f"Cevap telsizden (tasmanın imzalı): {done.outcome} · sürüm {done.revision} · "
                  f"aralık {done.interval_s} sn · Bluetooth {done.ble_window_s} sn")
        else:
            done = usb_exchange(tasma, cmd, key)
            print(f"Cevap (tasmanın imzalı): {done.outcome} · sürüm {done.revision} · "
                  f"aralık {done.interval_s} sn · Bluetooth {done.ble_window_s} sn")
            again = usb_exchange(tasma, cmd, key)
            print(f"Aynı komut tekrar: {again.outcome} · sürüm {again.revision} (değişmedi)"
                  if again == done else f"Aynı komut tekrar: {again}")
        return 0 if done.outcome in ("APPLIED", "UNCHANGED") else 1
    finally:
        if tasma is not None:
            tasma.close()
        if stopped_edge:
            print("PC Edge yeniden başlatılıyor…")
            subprocess.call(["systemctl", "--user", "start", PC_EDGE])


if __name__ == "__main__":
    sys.exit(main())
