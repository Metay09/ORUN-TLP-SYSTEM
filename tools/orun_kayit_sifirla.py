#!/usr/bin/env python3
"""Sunucudaki ve PC Edge'deki eski tasma kayıtlarını silip sıfırdan başlamak.

  python3 tools/orun_kayit_sifirla.py               # yalnız sayar, hiçbir şey değiştirmez
  python3 tools/orun_kayit_sifirla.py --tasma PORT  # tasmada gönderilmeyi bekleyen kayıt var mı (salt okunur)
  python3 tools/orun_kayit_sifirla.py --sil         # yedek alır, sonra siler ("SIL" yazınca)

SİLİNEN (yalnız kayıtlar):
  sunucu defteri   verified_observations, verified_conflicts, edge_receipts,
                   backend_uploads, edge_sessions
  PC Edge kutusu   sealed_records, gateway_holds
DOKUNULMAYAN: hesaplar, cihaz anahtarları ve izinleri (ayrı defter), cihaz
adları, pil kapasiteleri, alanlar, ayarlar ve bekleyen işlemler, harita paketleri.

--sil sırası: PC Edge ve sunucu durdurulur -> iki veritabanı SQLite yedek
API'siyle ~/ORUN-SIFIRLAMA-<zaman>/ klasörüne alınır ve bütünlüğü denetlenir ->
kayıt tabloları tek işlemde boşaltılır -> sunucu ve PC Edge yeniden başlar.
Yedek alınamazsa hiçbir şey silinmez ve servisler yeniden başlatılır.

Telefonlar: uygulama sunucudan "en son aldığım kayıt" sayısıyla çeker; sunucu
boşalınca bu sayı geçersiz olur. Silmeden sonra her telefonda uygulama verisi
temizlenip yeniden giriş yapılmalı (yoksa uygulama yeni kayıtları çekmez).
"""
from __future__ import annotations

import argparse
from contextlib import closing
import datetime as dt
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import time

SUNUCU = "orun-sunucu.service"
PC_EDGE = "orun-pc-edge.service"
SERVER_TABLES = ("verified_observations", "verified_conflicts", "edge_receipts",
                 "backend_uploads", "edge_sessions")
EDGE_TABLES = ("sealed_records", "gateway_holds")


def say(msg: str) -> None:
    print(msg, flush=True)


def unit_db(unit: str, marker: str, default: Path | None) -> Path | None:
    """--db of a systemd user service (its running command line, else ExecStart)."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import orun_pc_gecis as g  # noqa: E402 — same helpers orun_guncelle.sh uses
    argv, cwd = None, Path.home()
    try:
        live = g.live_argv(unit, marker)
        if live is not None:
            argv, cwd = live
        else:
            argv = g.exec_argv(g._show(unit))
    except Exception:  # noqa: BLE001 — the unit may not exist (no PC Edge)
        return default
    db = g.options(argv).get("db")
    if not isinstance(db, str):
        return default
    p = Path(os.path.expanduser(db))
    return p if p.is_absolute() else cwd / p


def tables(path: Path) -> set[str]:
    uri = path.resolve().as_uri() + "?mode=ro"
    with closing(sqlite3.connect(uri, uri=True, timeout=5)) as db:
        return {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}


def counts(path: Path, names: tuple[str, ...]) -> dict[str, int]:
    have = tables(path)
    uri = path.resolve().as_uri() + "?mode=ro"
    with closing(sqlite3.connect(uri, uri=True, timeout=5)) as db:
        return {t: db.execute(f"SELECT count(*) FROM {t}").fetchone()[0] for t in names if t in have}


def show(label: str, path: Path | None, names: tuple[str, ...]) -> None:
    if path is None or not path.is_file():
        say(f"{label}: bulunamadı ({path})")
        return
    c = counts(path, names)
    say(f"{label}: {path}")
    for t in names:
        say(f"  {t:<22} {c[t] if t in c else 'yok'}")


def backup(src: Path, dest: Path) -> None:
    with closing(sqlite3.connect(src.resolve().as_uri() + "?mode=ro", uri=True, timeout=10)) as s, \
         closing(sqlite3.connect(str(dest))) as d:
        s.backup(d)
        ok = d.execute("PRAGMA integrity_check").fetchone()[0]
    if ok != "ok":
        raise RuntimeError(f"yedek bütünlük denetimi: {ok}")
    os.chmod(dest, 0o600)


def wipe(path: Path, names: tuple[str, ...]) -> None:
    have = tables(path)
    with closing(sqlite3.connect(str(path), timeout=10)) as db:
        db.execute("BEGIN IMMEDIATE")
        for t in names:
            if t in have:
                db.execute(f"DELETE FROM {t}")
        db.execute("COMMIT")


def systemctl(*args: str) -> bool:
    return subprocess.run(["systemctl", "--user", *args], check=False).returncode == 0


def active(unit: str) -> bool:
    return subprocess.run(["systemctl", "--user", "is-active", "--quiet", unit], check=False).returncode == 0


def tracker_store(port: str) -> int:
    """PRODUCT STORE? of a collar on USB: records still waiting to be sent."""
    import serial  # pyserial, as the other device tools
    with serial.Serial(port, 115200, timeout=0.25, write_timeout=2, exclusive=True) as ser:
        ser.reset_input_buffer()
        ser.write(b"PRODUCT STORE?\n")
        end, lines = time.monotonic() + 4, []
        while time.monotonic() < end:
            line = ser.readline().decode("utf-8", "replace").strip()
            if line.startswith("PRODUCT STORE"):
                lines.append(line)
    if not lines:
        say("Tasma cevap vermedi (PRODUCT STORE?). Kabloyu çıkarıp tak, tekrar dene.")
        return 1
    for line in lines:
        say(line)
    status = next((l for l in lines if " retained=" in l), "")
    retained = next((int(p.split("=", 1)[1]) for p in status.split() if p.startswith("retained=")), None)
    if retained is None or retained < 0:
        say("Bekleyen kayıt sayısı okunamadı.")
    elif retained == 0:
        say("Tasmada gönderilmeyi bekleyen kayıt YOK: sunucu silinince eski kayıt geri gelmez.")
    else:
        say(f"Tasmada gönderilmeyi bekleyen {retained} kayıt VAR: sunucu silinse de bunlar "
            "gateway'e ulaşınca yeniden gelir.")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="ORUN eski kayıtları sıfırlama")
    ap.add_argument("--sil", action="store_true", help="yedek al ve kayıtları sil")
    ap.add_argument("--tasma", metavar="PORT", help="tasmada bekleyen kayıtları oku (salt okunur)")
    ap.add_argument("--db", type=Path, help="sunucu defteri (varsayılan: çalışan orun-sunucu'nunki)")
    ap.add_argument("--edge-db", type=Path, help="PC Edge kutusu (varsayılan: çalışan PC Edge'inki)")
    ap.add_argument("--servissiz", action="store_true", help=argparse.SUPPRESS)  # host test
    a = ap.parse_args(argv)

    if a.tasma:
        return tracker_store(a.tasma)

    server = a.db or unit_db(SUNUCU, "orun_sunucu.py", None)
    edge = a.edge_db or unit_db(PC_EDGE, "orun_pc_edge.py",
                                Path.home() / ".local/share/orun/pc-edge-inbox.sqlite")
    if server is None or not server.is_file():
        say(f"Sunucu defteri bulunamadı ({server}); --db ile ver. Hiçbir şey değişmedi.")
        return 1
    show("Sunucu defteri", server, SERVER_TABLES)
    show("PC Edge kutusu", edge, EDGE_TABLES)
    if not a.sil:
        say("\nYalnız sayıldı; hiçbir şey değişmedi. Silmek için: --sil")
        return 0

    say("\nYukarıdaki kayıtlar silinecek (önce yedek alınır). Hesaplar, anahtarlar, adlar,")
    say("alanlar ve ayarlar kalır. Devam etmek için SIL yaz:")
    if input("> ").strip() != "SIL":
        say("Vazgeçildi; hiçbir şey değişmedi.")
        return 0

    edge_ok = edge is not None and edge.is_file()
    was = {u: (not a.servissiz) and active(u) for u in (PC_EDGE, SUNUCU)}
    for u in (PC_EDGE, SUNUCU):
        if was[u]:
            say(f"Durduruluyor: {u}")
            if not systemctl("stop", u):
                say(f"{u} durdurulamadı; hiçbir şey silinmedi.")
                for v in (SUNUCU, PC_EDGE):
                    if was[v]:
                        systemctl("start", v)
                return 1

    def restart() -> None:
        for u in (SUNUCU, PC_EDGE):
            if was[u]:
                say(f"Başlatılıyor: {u}")
                systemctl("start", u)

    folder = Path.home() / f"ORUN-SIFIRLAMA-{dt.datetime.now():%Y%m%d-%H%M%S}"
    try:
        folder.mkdir(mode=0o700)
        backup(server, folder / server.name)
        if edge_ok:
            backup(edge, folder / edge.name)
        say(f"Yedek alındı ve denetlendi: {folder}")
    except Exception as exc:  # noqa: BLE001
        say(f"YEDEK ALINAMADI ({exc}); hiçbir şey silinmedi.")
        restart()
        return 1

    try:
        wipe(server, SERVER_TABLES)
        if edge_ok:
            wipe(edge, EDGE_TABLES)
    except Exception as exc:  # noqa: BLE001
        say(f"SİLME BAŞARISIZ ({exc}). Yedek: {folder}. Bu çıktıyı Claude'a gönder.")
        restart()
        return 1

    restart()
    if was[SUNUCU]:
        time.sleep(5)
        say("Sunucu çalışıyor." if active(SUNUCU) else f"UYARI: {SUNUCU} çalışmıyor; bu çıktıyı Claude'a gönder.")
    if was[PC_EDGE]:
        say("PC Edge çalışıyor." if active(PC_EDGE) else f"UYARI: {PC_EDGE} çalışmıyor; bu çıktıyı Claude'a gönder.")
    say("")
    show("Sunucu defteri (sonra)", server, SERVER_TABLES)
    show("PC Edge kutusu (sonra)", edge, EDGE_TABLES)
    say(f"\nTAMAM. Geri dönmek gerekirse yedek: {folder}")
    say("Şimdi her telefonda ORUN uygulamasının verisini temizle ve yeniden giriş yap.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
