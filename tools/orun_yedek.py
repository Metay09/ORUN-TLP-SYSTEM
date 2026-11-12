#!/usr/bin/env python3
"""ORUN sunucu yedeği ve yedekten dönüş provası.

  python3 tools/orun_yedek.py al        # çalışan orun-sunucu'nun verisini yedekle
  python3 tools/orun_yedek.py prova     # son yedeği AYRI geçici klasörde aç ve doğrula
  python3 tools/orun_yedek.py durum     # son yedek başarılı ve yeni mi?
  python3 tools/orun_yedek.py zamanlayici --kur   # her gece kendiliğinden yedek

Yedeklenenler (çalışan servisin kendi ayarından okunur): kayıt defteri (--db),
tasma/gateway kimlik ve anahtar defteri (--registry), hesap ve ortak veri
klasörü (--state-dir) ve köprü anahtarı dosyası. Harita paketleri yedeklenmez
(yeniden indirilir).

SQLite dosyaları kopyalanarak değil, SQLite'ın çevrim içi yedek API'siyle
alınır: sunucu çalışırken ve WAL'da yazılmamış kayıt varken de tutarlı tek bir
an görüntüsü verir. Her kopya bütünlük denetiminden geçer; her tablonun kayıt
sayısı ve her dosyanın SHA-256'sı yedeğin içindeki liste dosyasına yazılır.

Yedek gizli anahtar içerir: klasör 0700, dosyalar 0600, yalnız sahibine açık.
"prova" canlı veriye hiç dokunmaz: yedeği geçici bir klasöre kopyalar, sunucunun
kendi kodunu o kopyanın üstünde açar, kayıtları okur ve klasörü siler.
"""
from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import stat
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
SUNUCU = "orun-sunucu.service"
STAMP = re.compile(r"^\d{8}T\d{6}Z$")
MANIFEST = "LISTE.json"
STATUS = "SON_DURUM.json"
SQLITE_HEADER = b"SQLite format 3\x00"
SIDE_FILES = ("-wal", "-shm", "-journal")
MAX_PLAIN_FILE = 64 * 1024 * 1024


class YedekHatasi(RuntimeError):
    pass


# --------------------------------------------------------------------------
# Kaynaklar
# --------------------------------------------------------------------------

def unit_sources(unit: str) -> dict:
    """Çalışan servisin ExecStart satırından veri yolları (yalnız okur)."""
    sys.path.insert(0, str(ROOT / "tools"))
    import orun_pc_gecis as gecis  # aynı ayrıştırıcı; ikinci bir kopya yazılmaz
    shown = subprocess.run(
        ["systemctl", "--user", "show", "-p", "ExecStart", "--value", unit],
        capture_output=True, text=True, check=False)
    if shown.returncode != 0 or not shown.stdout.strip():
        raise YedekHatasi(f"{unit} bulunamadı; yolları elle ver "
                          "(--state-dir --db --registry --token-file --site)")
    o = gecis.options(gecis.exec_argv(shown.stdout))
    need = ("state-dir", "db", "registry", "site")
    if any(not isinstance(o.get(k), str) for k in need):
        raise YedekHatasi(f"{unit} ayarında " + ", ".join("--" + k for k in need) + " bekleniyordu")
    token = o.get("bridge-token-file")
    return {"state_dir": o["state-dir"], "db": o["db"], "registry": o["registry"],
            "site": o["site"], "token_file": token if isinstance(token, str) else None}


def sources_from(args) -> dict:
    explicit = [args.state_dir, args.db, args.registry, args.site]
    if any(explicit):
        if not all(explicit):
            raise YedekHatasi("--state-dir, --db, --registry ve --site birlikte verilmeli")
        return {"state_dir": args.state_dir, "db": args.db, "registry": args.registry,
                "site": args.site, "token_file": args.token_file}
    return unit_sources(args.unit)


# --------------------------------------------------------------------------
# Dosya yardımcıları
# --------------------------------------------------------------------------

def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def is_sqlite(path: Path) -> bool:
    try:
        with open(path, "rb") as f:
            return f.read(len(SQLITE_HEADER)) == SQLITE_HEADER
    except OSError:
        return False


def regular_private_source(path: Path, what: str) -> None:
    try:
        meta = path.lstat()
    except FileNotFoundError:
        raise YedekHatasi(f"{what} yok: {path}") from None
    if not stat.S_ISREG(meta.st_mode):
        raise YedekHatasi(f"{what} sıradan dosya değil: {path}")


def inspect_sqlite(path: Path) -> dict:
    """Bütünlük + tablo başına kayıt sayısı. Dosyayı değiştirmez."""
    with sqlite3.connect(f"file:{path}?mode=ro", uri=True, timeout=10) as db:
        check = [r[0] for r in db.execute("PRAGMA integrity_check")]
        if check != ["ok"]:
            raise YedekHatasi(f"bütünlük denetimi geçmedi: {path.name}: {check[:3]}")
        broken = db.execute("PRAGMA foreign_key_check").fetchall()
        if broken:
            raise YedekHatasi(f"kopuk ilişki: {path.name}: {len(broken)} satır")
        tables = {}
        for (name,) in db.execute(
                "SELECT name FROM sqlite_master WHERE type='table' "
                "AND name NOT LIKE 'sqlite_%' ORDER BY name"):
            quoted = '"' + name.replace('"', '""') + '"'
            tables[name] = db.execute(f"SELECT COUNT(*) FROM {quoted}").fetchone()[0]
    return tables


def snapshot_sqlite(source: Path, target: Path) -> dict:
    """WAL ile uyumlu, tek anlık kopya (sqlite3 çevrim içi yedek API'si)."""
    src = sqlite3.connect(f"file:{source}?mode=ro", uri=True, timeout=30)
    try:
        fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.close(fd)
        dst = sqlite3.connect(target)
        try:
            src.backup(dst)
            # Kopya tek dosya olsun: yanında -wal/-shm bırakma.
            dst.execute("PRAGMA journal_mode=DELETE")
            dst.commit()
        finally:
            dst.close()
    finally:
        src.close()
    os.chmod(target, 0o600)
    return inspect_sqlite(target)


def copy_plain(source: Path, target: Path) -> None:
    if source.stat().st_size > MAX_PLAIN_FILE:
        raise YedekHatasi(f"beklenmeyen büyük dosya (yedeklenmedi): {source}")
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as out, open(source, "rb") as src:
        shutil.copyfileobj(src, out)
        out.flush()
        os.fsync(out.fileno())


def state_files(state_dir: Path) -> list[Path]:
    """Hesap klasöründeki sıradan dosyalar (bağlar ve SQLite yan dosyaları hariç)."""
    out = []
    for path in sorted(state_dir.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        if path.name.endswith(SIDE_FILES):
            continue
        out.append(path)
    return out


# --------------------------------------------------------------------------
# al
# --------------------------------------------------------------------------

def write_status(root: Path, ok: bool, **fields) -> None:
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    data = {"ok": ok, "time_utc": int(time.time()), **fields}
    fd, name = tempfile.mkstemp(prefix=".durum-", dir=root)
    with os.fdopen(fd, "w") as f:
        json.dump(data, f)
        f.flush()
        os.fsync(f.fileno())
    os.replace(name, root / STATUS)


def backups(root: Path) -> list[Path]:
    if not root.is_dir():
        return []
    return sorted(p for p in root.iterdir()
                  if p.is_dir() and not p.is_symlink() and STAMP.fullmatch(p.name)
                  and (p / MANIFEST).is_file())


def take(src: dict, root: Path, keep: int, now: float | None = None) -> Path:
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(root, 0o700)
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime(now))
    final = root / stamp
    if final.exists():
        raise YedekHatasi(f"bu saniyede zaten bir yedek var: {final}")
    work = Path(tempfile.mkdtemp(prefix=f".{stamp}.yarim-", dir=root))
    try:
        files: dict[str, dict] = {}

        def add(source: Path, relative: str, role: str) -> None:
            target = work / relative
            target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            entry = {"role": role, "source": str(source)}
            if is_sqlite(source):
                entry["kind"] = "sqlite"
                entry["tables"] = snapshot_sqlite(source, target)
            else:
                entry["kind"] = "file"
                copy_plain(source, target)
            entry["bytes"] = target.stat().st_size
            entry["sha256"] = sha256(target)
            files[relative] = entry

        db, registry = Path(src["db"]), Path(src["registry"])
        state = Path(src["state_dir"])
        regular_private_source(db, "kayıt defteri")
        regular_private_source(registry, "kimlik defteri")
        if not state.is_dir():
            raise YedekHatasi(f"hesap klasörü yok: {state}")
        if not is_sqlite(db) or not is_sqlite(registry):
            raise YedekHatasi("--db ve --registry SQLite dosyası olmalı")
        add(db, "kayit_defteri.sqlite", "db")
        add(registry, "kimlik_defteri.sqlite", "registry")
        for path in state_files(state):
            add(path, "hesap/" + path.relative_to(state).as_posix(), "state")
        if "hesap/owner_accounts.sqlite" not in files:
            raise YedekHatasi("hesap klasöründe owner_accounts.sqlite yok")
        if src.get("token_file"):
            token = Path(src["token_file"])
            regular_private_source(token, "köprü anahtarı dosyası")
            add(token, "kopru_anahtari", "token")

        manifest = {"version": 1, "created_utc": int(now if now is not None else time.time()),
                    "site": src["site"], "files": files}
        with open(work / MANIFEST, "w") as f:
            json.dump(manifest, f, indent=1, sort_keys=True)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(work / MANIFEST, 0o600)
        # Yarım yedek asla tam yedek gibi görünmesin: ad en sonda verilir.
        os.rename(work, final)
    except BaseException:
        shutil.rmtree(work, ignore_errors=True)
        raise
    for old in backups(root)[:-keep] if keep > 0 else []:
        shutil.rmtree(old)
    return final


# --------------------------------------------------------------------------
# prova
# --------------------------------------------------------------------------

def load_manifest(backup: Path) -> dict:
    try:
        manifest = json.loads((backup / MANIFEST).read_text())
    except (OSError, ValueError) as exc:
        raise YedekHatasi(f"yedek listesi okunamadı: {exc}") from None
    if manifest.get("version") != 1 or not isinstance(manifest.get("files"), dict):
        raise YedekHatasi("yedek listesi tanınmadı")
    return manifest


def verify_files(backup: Path, manifest: dict) -> None:
    for relative, entry in manifest["files"].items():
        path = backup / relative
        if path.is_symlink() or not path.is_file():
            raise YedekHatasi(f"yedekte eksik dosya: {relative}")
        if path.stat().st_size != entry["bytes"] or sha256(path) != entry["sha256"]:
            raise YedekHatasi(f"yedek dosyası değişmiş ya da bozuk: {relative}")
    extra = {p.relative_to(backup).as_posix() for p in backup.rglob("*") if p.is_file()}
    extra -= set(manifest["files"]) | {MANIFEST}
    if extra:
        raise YedekHatasi("yedekte listede olmayan dosya: " + ", ".join(sorted(extra)[:3]))


def relations(registry: Path) -> list[str]:
    """Kimlik defterinde birbirine bağlı kayıtlar tutarlı mı? (yalnız okur)"""
    problems = []
    with sqlite3.connect(f"file:{registry}?mode=ro", uri=True) as db:
        names = {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}

        def count(sql: str) -> int:
            return db.execute(sql).fetchone()[0]

        if {"grants", "credentials"} <= names and count(
                "SELECT COUNT(*) FROM grants g WHERE NOT EXISTS (SELECT 1 FROM credentials c "
                "WHERE c.device_id=g.device_id AND c.key_epoch=g.key_epoch)"):
            problems.append("kimliği olmayan tasmaya verilmiş gateway izni var")
        if {"gateway_edge_keys", "farm_gateways"} <= names and count(
                "SELECT COUNT(*) FROM gateway_edge_keys k WHERE NOT EXISTS "
                "(SELECT 1 FROM farm_gateways f WHERE f.gateway_id=k.gateway_id)"):
            problems.append("çiftlikte olmayan gateway için teslim anahtarı var")
        if {"grants", "issuer"} <= names and count("SELECT COUNT(*) FROM grants") \
                and not count("SELECT COUNT(*) FROM issuer"):
            problems.append("gateway izni var ama izinleri imzalayan anahtar yok")
    return problems


def rehearse(backup: Path, log=print) -> dict:
    manifest = load_manifest(backup)
    verify_files(backup, manifest)
    log(f"1/4 Yedek dosyaları eksiksiz ve değişmemiş ({len(manifest['files'])} dosya).")

    work = Path(tempfile.mkdtemp(prefix="orun-prova-"))
    os.chmod(work, 0o700)
    try:
        for relative in manifest["files"]:
            target = work / relative
            target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            shutil.copyfile(backup / relative, target)
            os.chmod(target, 0o600)
        for folder in [p for p in work.rglob("*") if p.is_dir()]:
            os.chmod(folder, 0o700)

        totals = {}
        for relative, entry in manifest["files"].items():
            if entry["kind"] != "sqlite":
                continue
            tables = inspect_sqlite(work / relative)
            if tables != entry["tables"]:
                raise YedekHatasi(f"kayıt sayıları tutmuyor: {relative}")
            totals[relative] = sum(tables.values())
        log("2/4 Veritabanları sağlam; her tablonun kayıt sayısı yedek listesiyle aynı.")

        problems = relations(work / "kimlik_defteri.sqlite")
        if problems:
            raise YedekHatasi("ilişkiler: " + "; ".join(problems))
        log("3/4 Tasma kimlikleri, gateway izinleri ve teslim anahtarları birbirine bağlı.")

        report = _server_reads(work, manifest, totals)
        log("4/4 Sunucu yazılımı geri yüklenen kopyayı açtı: "
            f"{report['records']} kayıt, {report['latest']} tasmanın son konumu, "
            f"{report['keys']} tasma anahtarı, {report['users']} hesap okundu.")
        return report
    finally:
        shutil.rmtree(work, ignore_errors=True)


def _server_reads(work: Path, manifest: dict, totals: dict) -> dict:
    """Sunucunun kendi kodunu geri yüklenen kopyada çalıştır (canlı yol YOK)."""
    live = {entry["source"] for entry in manifest["files"].values()}
    if any(str(p) in live for p in work.rglob("*")):
        raise YedekHatasi("prova klasörü canlı veriyle çakışıyor")  # savunma
    if str(ROOT) not in sys.path:
        sys.path.insert(0, str(ROOT))
    from http.server import ThreadingHTTPServer
    from tools.orun_sunucu import bridge_handler, build, load_private_token_file

    maps = work / "bos-harita"
    maps.mkdir(mode=0o700)
    local, _handler = build(work / "hesap", work / "kayit_defteri.sqlite",
                            work / "kimlik_defteri.sqlite", manifest["site"],
                            maps_dir=maps)
    ledger = manifest["files"]["kayit_defteri.sqlite"]["tables"]
    records = local.backend.ledger.record_count()
    expected = ledger.get("verified_observations")
    if expected is not None and records != expected:
        raise YedekHatasi(f"sunucu {records} kayıt görüyor, yedekte {expected} vardı")
    status, latest = local.forward("GET", "/v1/edge/records?latest=1")
    if status != 200:
        raise YedekHatasi(f"sunucu kayıtları okuyamadı (durum {status})")
    if records and not latest.get("records"):
        raise YedekHatasi("kayıt var ama sunucu son konumları veremedi")
    keys = len(local.keys()["keys"])
    with sqlite3.connect(f"file:{work / 'hesap' / 'owner_accounts.sqlite'}?mode=ro",
                         uri=True) as db:
        users = db.execute("SELECT COUNT(*) FROM account_users").fetchone()[0]
    if users < 1:
        raise YedekHatasi("geri yüklenen kopyada hesap yok; uygulamaya giriş yapılamaz")

    token_path = work / "kopru_anahtari"
    if token_path.is_file():
        token = load_private_token_file(str(token_path))
        server = ThreadingHTTPServer(("127.0.0.1", 0), bridge_handler(local, token))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            conn = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=10)
            conn.request("GET", "/v1/health", headers={"Authorization": "Bearer " + token})
            reply = conn.getresponse()
            body = json.loads(reply.read() or b"{}")
            conn.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join(5)
        if reply.status != 200 or body.get("records") != records:
            raise YedekHatasi("köprü adresi geri yüklenen kopyada cevap vermedi")
    return {"records": records, "latest": len(latest.get("records", [])),
            "keys": keys, "users": users, "rows": totals}


# --------------------------------------------------------------------------
# Komut satırı
# --------------------------------------------------------------------------

def default_root() -> Path:
    return Path.home() / "ORUN-YEDEK"


def timer_units(script: Path, python: str) -> tuple[str, str]:
    service = ("[Unit]\nDescription=ORUN sunucu yedegi\n\n[Service]\nType=oneshot\n"
               f"ExecStart={python} {script} al\n")
    timer = ("[Unit]\nDescription=ORUN sunucu yedegi (her gece)\n\n[Timer]\n"
             "OnCalendar=*-*-* 03:17:00\nPersistent=true\n\n[Install]\nWantedBy=timers.target\n")
    return service, timer


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="ORUN sunucu yedeği ve yedekten dönüş provası")
    sub = ap.add_subparsers(dest="cmd", required=True)
    al = sub.add_parser("al", help="yedek al")
    al.add_argument("--unit", default=SUNUCU)
    for name in ("--state-dir", "--db", "--registry", "--token-file", "--site"):
        al.add_argument(name)
    al.add_argument("--hedef", type=Path, default=None)
    al.add_argument("--sakla", type=int, default=14, help="kaç yedek tutulsun (varsayılan 14)")
    pr = sub.add_parser("prova", help="yedeği ayrı geçici klasörde aç ve doğrula")
    pr.add_argument("--yedek", type=Path, help="yedek klasörü (varsayılan: en yenisi)")
    pr.add_argument("--hedef", type=Path, default=None)
    du = sub.add_parser("durum", help="son yedek başarılı ve yeni mi")
    du.add_argument("--hedef", type=Path, default=None)
    du.add_argument("--en-fazla-saat", type=float, default=26.0)
    za = sub.add_parser("zamanlayici", help="her gece 03:17'de kendiliğinden yedek")
    za.add_argument("--kur", action="store_true")
    args = ap.parse_args(argv)
    root = getattr(args, "hedef", None) or default_root()

    try:
        if args.cmd == "al":
            if args.sakla < 1:
                raise YedekHatasi("--sakla en az 1 olmalı")
            try:
                final = take(sources_from(args), root, args.sakla)
            except (YedekHatasi, OSError, sqlite3.Error) as exc:
                write_status(root, False, error=str(exc))
                raise
            manifest = load_manifest(final)
            write_status(root, True, backup=final.name)
            size = sum(e["bytes"] for e in manifest["files"].values())
            print(f"YEDEK TAMAM: {final}")
            print(f"  {len(manifest['files'])} dosya, {size / 1e6:.1f} MB; "
                  f"{len(backups(root))} yedek tutuluyor (en çok {args.sakla}).")
            for relative, entry in manifest["files"].items():
                if entry["kind"] == "sqlite":
                    print(f"  {relative}: {sum(entry['tables'].values())} kayıt")
            return 0
        if args.cmd == "prova":
            backup = args.yedek
            if backup is None:
                found = backups(root)
                if not found:
                    raise YedekHatasi(f"{root} içinde yedek yok (önce: al)")
                backup = found[-1]
            print(f"PROVA: {backup} (canlı veriye dokunulmaz)")
            rehearse(backup)
            print("PROVA TAMAM: bu yedekten dönülebilir.")
            return 0
        if args.cmd == "durum":
            try:
                state = json.loads((root / STATUS).read_text())
            except (OSError, ValueError):
                print(f"YEDEK YOK: {root} içinde durum kaydı bulunamadı.")
                return 1
            age = (time.time() - state.get("time_utc", 0)) / 3600
            if not state.get("ok"):
                print(f"SON YEDEK BAŞARISIZ ({age:.1f} saat önce): {state.get('error')}")
                return 1
            if age > args.en_fazla_saat:
                print(f"YEDEK ESKİ: son başarılı yedek {age:.1f} saat önce ({state.get('backup')}).")
                return 1
            print(f"YEDEK GÜNCEL: {state.get('backup')} ({age:.1f} saat önce).")
            return 0
        service, timer = timer_units(Path(__file__).resolve(), sys.executable)
        if not args.kur:
            print("--- orun-yedek.service ---\n" + service)
            print("--- orun-yedek.timer ---\n" + timer)
            print("Kurmak için: python3 tools/orun_yedek.py zamanlayici --kur")
            return 0
        units = Path.home() / ".config/systemd/user"
        units.mkdir(parents=True, exist_ok=True)
        (units / "orun-yedek.service").write_text(service)
        (units / "orun-yedek.timer").write_text(timer)
        subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
        subprocess.run(["systemctl", "--user", "enable", "--now", "orun-yedek.timer"], check=True)
        print("Kuruldu: her gece 03:17'de yedek alınır. Denetim: python3 tools/orun_yedek.py durum")
        return 0
    except (YedekHatasi, OSError, sqlite3.Error, subprocess.CalledProcessError) as exc:
        print(f"DUR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
