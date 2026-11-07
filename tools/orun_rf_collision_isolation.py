#!/usr/bin/env python3
"""ORUN RF collision isolation: 4 min one-tracker test, automatic restore.

Only mutable command: BENCH DRAIN OFF on 141A8887, then BENCH DRAIN ON.
No flash, erase, reset, provisioning, service stop, or gateway serial access.
"""
import os
import re
import signal
import sqlite3
import subprocess
import threading
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
except ImportError:
    raise SystemExit("HATA: pyserial gerekli (python3-serial)")

HOME = Path.home()
DEVICE = {
    "HAYVAN 2": "0E8ADE7E71531AA3",
    "TASMA 141A8887": "52E16773141A8887",
}
QUIET = "TASMA 141A8887"
DURATION = 240
AFTER = 90
LOG = HOME / ("orun_rf_izolasyon_" + datetime.now().strftime("%Y%m%d_%H%M%S") + ".log")
EDGE = HOME / ".local/share/orun/pc-edge-inbox.sqlite"
stop = threading.Event()
print_lock = threading.Lock()
links = {}
journal = None
restore_needed = False
phase = "HAZIRLIK"
metrics = {"HAYVAN 2": {"tx": 0, "ack": 0, "timeout": 0},
           "TASMA 141A8887": {"tx": 0, "ack": 0, "timeout": 0}}
logfile = None

def emit(who, msg):
    line = (datetime.now().strftime("%H:%M:%S") + " | " + phase +
            " | " + who.ljust(14) + " | " + str(msg))
    with print_lock:
        print(line, flush=True)
        if logfile is not None and not logfile.closed:
            logfile.write(line + "\n")
            logfile.flush()

def port_path(ident):
    return Path("/dev/serial/by-id") / (
        "usb-RAKwireless_WisCore_RAK4631_Board_" + ident + "-if00"
    )

def connect(name):
    path = port_path(DEVICE[name])
    if not path.exists():
        raise RuntimeError(name + ": USB aygiti bulunamadi")
    busy = subprocess.run(
        ["fuser", "-s", os.path.realpath(path)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    if busy.returncode == 0:
        raise RuntimeError(name + ": USB portu baska program tarafindan kullaniliyor")
    if busy.returncode not in (0, 1):
        raise RuntimeError(name + ": port kullanim durumu dogrulanamadi")
    s = serial.Serial(port=None, baudrate=115200, timeout=0.15,
                      write_timeout=2, exclusive=True)
    s.dtr = True
    s.rts = False
    s.port = str(path)
    s.open()
    s.dtr = True
    time.sleep(0.8)
    emit(name, "USB acildi; DTR=ON")
    return s

def line_event(name, line):
    if line.startswith("BENCH LINK tx "):
        metrics[name]["tx"] += 1
    elif line.startswith("BENCH LINK ack seq="):
        metrics[name]["ack"] += 1
    elif line.startswith("BENCH LINK ack_timeout "):
        metrics[name]["timeout"] += 1
    if line.startswith((
        "BENCH LINK", "BENCH DRAIN", "BENCH PILOT",
        "PRODUCT CUSTODY release=", "PRODUCT PILOT blocked",
        "PRODUCT PILOT TX=start",
    )):
        emit(name, line[:340])

def read_status(name, cmd="BENCH DRAIN?", seconds=3.5):
    s = links[name]
    s.write((cmd + "\n").encode("ascii"))
    deadline = time.monotonic() + seconds
    mode = None
    while time.monotonic() < deadline:
        raw = s.readline()
        if not raw:
            continue
        line = raw.decode("ascii", "replace").strip()
        line_event(name, line)
        match = re.match(r"^BENCH DRAIN (ON|OFF|LIVE)\b", line)
        if match:
            mode = match[1]
            break
    if mode is None:
        raise RuntimeError(name + " " + cmd + ": USB yaniti yok")
    return mode

def gateway_watch():
    global journal
    args = ["journalctl", "--user", "-u", "orun-pc-edge.service",
            "-f", "-n", "0", "--no-pager", "-o", "cat"]
    try:
        journal = subprocess.Popen(
            args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, bufsize=1
        )
        last = None
        for raw in journal.stdout:
            if stop.is_set():
                break
            s = raw.strip()
            match = re.search(r"custody_started=(\d+).*?ack_tx=(\d+)", s)
            if match:
                now = (int(match[1]), int(match[2]))
                d = "ilk kontrol" if last is None else (
                    "delta_custody=%+d delta_ack_tx=%+d" %
                    (now[0] - last[0], now[1] - last[1])
                )
                emit("GATEWAY", "custody=%d ack_tx=%d %s" % (*now, d))
                last = now
            elif ("[backend] confirmed" in s or
                  "[usb] disconnected" in s or
                  "[usb] Gateway connected" in s):
                emit("PC EDGE", s[:220])
    except Exception as exc:
        emit("GATEWAY", "gunluk izleme hatasi: " + repr(exc))

def edge_watch():
    if not EDGE.exists():
        emit("PC EDGE", "veritabani bulunamadi")
        return
    try:
        db = sqlite3.connect(
            EDGE.resolve().as_uri() + "?mode=ro", uri=True, timeout=1
        )
        db.execute("PRAGMA query_only=ON")
        last = db.execute("SELECT COALESCE(MAX(id),0) FROM sealed_records").fetchone()[0]
        emit("PC EDGE", "yeni paket baslangic_id=%d" % last)
        while not stop.is_set():
            rows = db.execute(
                "SELECT id,frame,first_seen_utc FROM sealed_records "
                "WHERE id>? ORDER BY id LIMIT 200", (last,)
            ).fetchall()
            for rid, frame, stamp in rows:
                last = rid
                frame = bytes(frame)
                ident = frame[8:16].hex().upper() if len(frame) >= 28 else ""
                found = next((name for name, d in DEVICE.items() if d == ident), None)
                if found:
                    counter = int.from_bytes(frame[20:28], "big")
                    stamp_str = datetime.fromtimestamp(stamp).strftime("%H:%M:%S")
                    emit("PC EDGE", "%s id=%d counter=%d ilk_kayit=%s" %
                         (found, rid, counter, stamp_str))
            stop.wait(1)
        db.close()
    except Exception as exc:
        emit("PC EDGE", "veritabani izleme hatasi: " + repr(exc))

def observe(seconds):
    next_status = time.monotonic() + 30
    limit = time.monotonic() + seconds
    while time.monotonic() < limit:
        for name, s in links.items():
            raw = s.readline()
            if raw:
                line_event(name, raw.decode("ascii", "replace").strip())
        if time.monotonic() >= next_status:
            for name in links:
                read_status(name)
            next_status = time.monotonic() + 30

def restore():
    global restore_needed
    if not restore_needed:
        return True
    for attempt in range(1, 4):
        try:
            if QUIET not in links or not links[QUIET].is_open:
                links[QUIET] = connect(QUIET)
            mode = read_status(QUIET, "BENCH DRAIN ON", seconds=4)
            if mode == "ON":
                restore_needed = False
                emit(QUIET, "GERI ACILDI: BENCH DRAIN ON dogrulandi")
                return True
            emit(QUIET, "beklenmeyen geri-acma modu: " + mode)
        except Exception as exc:
            emit(QUIET, "geri acma denemesi %d basarisiz: %s" %
                 (attempt, exc))
            try:
                links[QUIET].close()
            except Exception:
                pass
            time.sleep(1)
    emit(QUIET, "ACIL: otomatik geri acma DOGRULANAMADI! "
         "USB terminalden BENCH DRAIN ON uygulanmali.")
    return False

def main():
    global logfile, phase, restore_needed
    fd = os.open(LOG, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    logfile = os.fdopen(fd, "w", buffering=1)
    print("=== ORUN RF CARPISMA IZOLASYON TESTI ===")
    print("4 dk tek tasma + 90 sn sonrasi, Ctrl+C ile kesilebilir.")
    print("Log:", LOG)
    try:
        for unit in ("orun-pc-edge.service", "orun-sunucu.service"):
            state = subprocess.run(
                ["systemctl", "--user", "is-active", unit],
                capture_output=True, text=True
            ).stdout.strip()
            if state != "active":
                raise RuntimeError(unit + " aktif degil: " + state)
        for name in DEVICE:
            links[name] = connect(name)
        a = read_status("HAYVAN 2")
        b = read_status(QUIET)
        if a != "ON" or b != "ON":
            raise RuntimeError("Her iki cihaz BENCH DRAIN ON olmali: "
                               "Hayvan2=%s diger=%s. Degisiklik yapilmadi." %
                               (a, b))
        emit("TEST", "Baslangic: iki tasma ON; gateway portuna dokunulmayacak")
        threading.Thread(target=gateway_watch, daemon=True).start()
        threading.Thread(target=edge_watch, daemon=True).start()
        phase = "TEK TASMA"
        restore_needed = True  # Set BEFORE sending OFF, even if response is lost.
        result = read_status(QUIET, "BENCH DRAIN OFF", seconds=4)
        if result != "OFF":
            raise RuntimeError("OFF yaniti dogrulanamadi: " + result)
        emit("TEST", "141A8887 guvenli drain OFF. Hayvan 2: 240 saniye izleniyor")
        observe(DURATION)
        phase = "GERI ACMA"
        if not restore():
            return 2
        emit("TEST", "Iki tasma yeniden ON; 90 saniye daha izleniyor")
        phase = "SON KONTROL"
        observe(AFTER)
        return 0
    except KeyboardInterrupt:
        emit("TEST", "Ctrl+C algilandi; geri acma yapilacak")
        return 130
    except Exception as exc:
        emit("TEST", "HATA: " + repr(exc))
        return 1
    finally:
        phase = "GERI ACMA"
        ok = restore()
        if not ok:
            print("DIKKAT: TASMA 141A8887 BENCH DRAIN ON elle yapilmali!", flush=True)
        phase = "OZET"
        for name, stat in metrics.items():
            emit(name, "GORULEN tx=%d ack=%d timeout=%d" %
                 (stat["tx"], stat["ack"], stat["timeout"]))
        stop.set()
        if journal is not None and journal.poll() is None:
            journal.terminate()
            try:
                journal.wait(timeout=2)
            except subprocess.TimeoutExpired:
                journal.kill()
        for s in links.values():
            try:
                s.close()
            except Exception:
                pass
        emit("TEST", "Log: " + str(LOG))
        logfile.close()

def on_terminate(signum, frame):
    raise KeyboardInterrupt

if __name__ == "__main__":
    signal.signal(signal.SIGTERM, on_terminate)
    raise SystemExit(main())
