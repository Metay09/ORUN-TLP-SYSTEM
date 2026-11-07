#!/usr/bin/env python3
"""ORUN 3-device read-only timeline. Two tracker USB serial monitors + Gateway/PC Edge logs and verified backend records."""
import os
import queue
import re
import sqlite3
import subprocess
import threading
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
except ImportError:
    raise SystemExit('HATA: python3-serial (pyserial) kurulu degil.')

DURATION = 600  # 10 minutes
DEVICES = {
    '0E8ADE7E71531AA3': 'HAYVAN 2',
    '52E16773141A8887': 'TASMA 141A8887',
}
BASE = Path.home()
EDGE = BASE / '.local/share/orun/pc-edge-inbox.sqlite'
BACKEND = BASE / 'orun-backend-bench.sqlite'
OUT = BASE / ('orun_3cihaz_' + datetime.now().strftime('%Y%m%d_%H%M%S') + '.log')
STOP = threading.Event()
JOURNAL = {}
EVENTS = queue.Queue()
START_TIME = time.time()
END = time.monotonic() + DURATION

def emit(who, message, when=None):
    clock = datetime.fromtimestamp(time.time() if when is None else when).strftime('%H:%M:%S')
    EVENTS.put(f'{clock} | {who:<14} | {message}')

def serial_watch(dev, name):
    path = Path('/dev/serial/by-id') / f'usb-RAKwireless_WisCore_RAK4631_Board_{dev}-if00'
    if not path.exists():
        emit(name, 'HATA: cihaz USB portunda yok')
        return
    busy = subprocess.run(['fuser', '-s', os.path.realpath(path)],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    if busy:
        emit(name, 'HATA: seri port baska program tarafindan kullaniliyor')
        return
    try:
        link = serial.Serial(port=None, baudrate=115200, timeout=0.25,
                             write_timeout=2, exclusive=True)
        link.dtr = False
        link.rts = False
        link.port = str(path)
        link.open()
        with link:
            emit(name, 'USB baglandi; sadece tani okumalari')
            link.write(b'BENCH DRAIN?\n')
            diag = ('BENCH DRAIN', 'BENCH PILOT')
            significant = ('BENCH LINK', 'PRODUCT CUSTODY release=',
                           'PRODUCT PILOT blocked', 'PRODUCT PILOT TX=start',
                           'GNSS FIX')
            while not STOP.is_set() and time.monotonic() < END:
                raw = link.readline()
                if not raw:
                    continue
                line = raw.decode('ascii', 'replace').strip()
                if line.startswith(diag + significant):
                    emit(name, line[:320])
            link.write(b'BENCH DRAIN?\n')
            until = time.monotonic() + 3
            while time.monotonic() < until:
                raw = link.readline()
                if raw:
                    line = raw.decode('ascii', 'replace').strip()
                    if line.startswith(diag):
                        emit(name, 'SON ' + line[:300])
    except Exception as exc:
        emit(name, f'USB HATA: {type(exc).__name__}: {exc}')

def gateway_watch():
    args = ['journalctl', '--user', '-u', 'orun-pc-edge.service',
            '-f', '-n', '0', '--no-pager', '-o', 'cat']
    prev = None
    try:
        proc = subprocess.Popen(args, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL,
                                text=True, bufsize=1)
        JOURNAL['proc'] = proc
        with proc:
            emit('GATEWAY', 'PC Edge gunlugu izleniyor; gateway USB portu acilmadi')
            for raw in proc.stdout:
                if STOP.is_set():
                    break
                line = raw.strip()
                if 'BENCH GATEWAY custody_started=' in line:
                    match = re.search(r'custody_started=(\d+).*?ack_tx=(\d+)', line)
                    if match:
                        current = tuple(map(int, match.groups()))
                        delta = ('ilk olcum' if prev is None else
                                 f'(+{current[0]-prev[0]} kabul, +{current[1]-prev[1]} ACK-TX)')
                        emit('GATEWAY', f'custody={current[0]}, ack_tx={current[1]} {delta}')
                        prev = current
                elif ('[usb] disconnected' in line or '[usb] Gateway connected' in line or
                      '[backend] confirmed' in line or '[backend] error' in line or
                      '[backend] failed' in line):
                    emit('PC EDGE', line[:220])
    except Exception as exc:
        emit('GATEWAY', f'GUNLUK HATA: {type(exc).__name__}: {exc}')

def open_readonly(path):
    c = sqlite3.connect(path.resolve().as_uri() + '?mode=ro',
                        uri=True, timeout=1, check_same_thread=True)
    c.execute('PRAGMA query_only=ON')
    c.execute('PRAGMA busy_timeout=1000')
    return c

def records_watch():
    if not EDGE.exists():
        emit('PC EDGE', f'HATA: veritabani bulunamadi: {EDGE}')
        return
    try:
        ec = open_readonly(EDGE)
        bc = open_readonly(BACKEND) if BACKEND.exists() else None
        if bc is None:
            emit('BACKEND', f'Uyari: veritabani bulunamadi: {BACKEND}')
        last = ec.execute('SELECT COALESCE(MAX(id),0) FROM sealed_records').fetchone()[0]
        emit('PC EDGE', f'yeni paketler izleniyor; baslangic_id={last}')
        pending = {}  # id -> (device, received, frame, first_poll)
        while not STOP.is_set() and time.monotonic() < END:
            rows = ec.execute('''SELECT id,frame,first_seen_utc
                                 FROM sealed_records WHERE id>? ORDER BY id LIMIT 200''',
                              (last,)).fetchall()
            for rid, buf, first in rows:
                last = rid
                frame = bytes(buf)
                if len(frame) < 28:
                    continue
                dev = frame[8:16].hex().upper()
                if dev not in DEVICES:
                    continue
                counter = int.from_bytes(frame[20:28], 'big')
                label = DEVICES[dev]
                emit('PC EDGE', f'{label} kabul: counter={counter} id={rid}', first)
                pending[rid] = (label, first, frame, time.monotonic())
            if bc:
                for rid, (label, first, frame, since) in list(pending.items()):
                    try:
                        found = bc.execute('''SELECT sequence, plaintext FROM verified_observations
                                              WHERE exact_first_frame=? LIMIT 1''', (frame,)).fetchone()
                    except sqlite3.OperationalError:
                        found = None
                    if found:
                        seq, plain = found
                        plain = bytes(plain)
                        if len(plain) == 64:
                            stamp = int.from_bytes(plain[16:20], 'big') if plain[1] & 1 else 0
                            duration = int.from_bytes(plain[20:24], 'big')
                            delay = f'{int(first - stamp)}s' if stamp else '?'
                            measured = datetime.fromtimestamp(stamp).strftime('%H:%M:%S') if stamp else '?'
                            emit('DOGRULANDI', f'{label} seq={seq} olcum={measured} gecikme={delay} periyot={duration}s')
                        else:
                            emit('DOGRULANDI', f'{label} seq={seq} (olay)')
                        del pending[rid]
                    elif time.monotonic() - since > 30:
                        emit('BACKEND', f'{label} id={rid} 30sn icinde dogrulanmadi (sonra da gelebilir)')
                        del pending[rid]
            if not rows:
                STOP.wait(1)
    except Exception as exc:
        emit('PC EDGE', f'VERITABANI HATA: {type(exc).__name__}: {exc}')

threads = [
    threading.Thread(target=serial_watch, args=(dev, name), daemon=True)
    for dev, name in DEVICES.items()
]
threads += [threading.Thread(target=gateway_watch, daemon=True),
            threading.Thread(target=records_watch, daemon=True)]

fd = os.open(OUT, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(fd, 'w', buffering=1) as logfile:
    print('=== ORUN 3 CIHAZ TEK ZAMAN CIZELGESI ===', flush=True)
    print('Sure: 10 dakika | Ctrl+C: erken bitir | Log:', OUT, flush=True)
    print('Gateway seri portuna dokunulmaz. Saatler PC saatidir.\n', flush=True)
    for thread in threads:
        thread.start()
    try:
        while time.monotonic() < END:
            try:
                line = EVENTS.get(timeout=0.3)
            except queue.Empty:
                continue
            print(line, flush=True)
            logfile.write(line + '\n')
    except KeyboardInterrupt:
        print('\nKullanici izlemeyi durdurdu.', flush=True)
    finally:
        STOP.set()
        proc = JOURNAL.get('proc')
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        for th in threads[:2]:
            th.join(timeout=4)
        while not EVENTS.empty():
            line = EVENTS.get_nowait()
            print(line, flush=True)
            logfile.write(line + '\n')
        print('\nBitti. Kaydedilen log:', OUT, flush=True)