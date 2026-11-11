#!/usr/bin/env python3
"""6a masa denemesi: sunucunun imzalayacağı LoRa ayar komutunu USB'den ver.

Komut tasmaya LoRa'dan gelecekmiş gibi aynı yoldan işlenir (firmware
LoraCommandService); 6b'de aynı baytlar gateway'den gelecek. Önce bilerek
eski durumla bir komut gider: tasma "STALE" der ve imzalı cevabında güncel
durumunu söyler; araç komutu o duruma göre yeniden imzalayıp gönderir.

  python3 tools/orun_lora_komut.py --port /dev/serial/by-id/...-if00 --aralik 300
  python3 tools/orun_lora_komut.py --port ... --aralik 600 --ble 60
  python3 tools/orun_lora_komut.py --port ... --aralik 180 --ble 30   # geri al
"""
from __future__ import annotations

import argparse
from pathlib import Path
import secrets
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from backend import lora_command as lc  # noqa: E402
from backend import orun_credentials as oc  # noqa: E402


def exchange(ser, frame: bytes, key: bytes, timeout: float = 12.0) -> lc.Result:
    ser.reset_input_buffer()
    ser.write(b"APP LORA CMD " + frame.hex().upper().encode() + b"\n")
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = ser.readline().decode(errors="replace").strip()
        if line.startswith("APP LORA via=") and "PENDING" not in line:
            raise SystemExit(f"Tasma komutu almadı: {line}")
        if line.startswith("APP LORA RESULT "):
            hexpart = line.rsplit("hex=", 1)[-1]
            return lc.parse_result(key, bytes.fromhex(hexpart))
    raise SystemExit("Tasmadan cevap gelmedi.")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--registry", default=str(Path.home() / "orun-credentials.sqlite"))
    ap.add_argument("--aralik", type=int, help="rapor aralığı (sn)")
    ap.add_argument("--ble", type=int, help="rapordan sonra Bluetooth (sn)")
    a = ap.parse_args()
    entries = ([(1, a.aralik)] if a.aralik is not None else []) + ([(3, a.ble)] if a.ble is not None else [])
    if not entries:
        ap.error("--aralik ya da --ble ver")
    import serial  # pyserial

    port_id = Path(a.port).name.split("_Board_")[-1].split("-if")[0]
    reg = oc.CredentialRegistry(a.registry)
    cred = reg.active(int(port_id, 16)) if len(port_id) == 16 else None
    if cred is None:
        raise SystemExit(f"Bu tasmanın anahtarı kayıtlarda yok ({port_id}). --port by-id yolu olmalı.")
    key = oc.ble_write_key(cred)
    with serial.Serial(a.port, 115200, timeout=0.5, write_timeout=2, exclusive=True) as ser:
        # 1) Durumu öğren: bilerek eski durumla (STALE cevabı güncel durumu taşır).
        probe = lc.build_command(key, cred.device_id, cred.key_epoch, secrets.randbits(32), 0, 0, entries)
        now = exchange(ser, probe, key)
        if now.outcome != "STALE":
            raise SystemExit(f"Beklenmeyen cevap: {now}")
        print(f"Tasmanın durumu: sürüm {now.revision}, aralık {now.interval_s} sn, "
              f"Bluetooth {now.ble_window_s} sn")
        # 2) Gerçek komut, o duruma bağlı.
        cmd = lc.build_command(key, cred.device_id, cred.key_epoch, secrets.randbits(32),
                               now.incarnation, now.revision, entries)
        done = exchange(ser, cmd, key)
        print(f"Cevap (tasmanın imzalı): {done.outcome} · sürüm {done.revision} · "
              f"aralık {done.interval_s} sn · Bluetooth {done.ble_window_s} sn")
        # 3) Aynı komut tekrar: tasma aynı cevabı verir, hiçbir şey değişmez.
        again = exchange(ser, cmd, key)
        print(f"Aynı komut tekrar: {again.outcome} · sürüm {again.revision} (değişmedi)"
              if again == done else f"Aynı komut tekrar: {again}")
    return 0 if done.outcome in ("APPLIED", "UNCHANGED") else 1


if __name__ == "__main__":
    sys.exit(main())
