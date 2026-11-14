#!/usr/bin/env python3
"""ORUN hesabının kullanıcı adını gösterir ve parolasını yeniler (yalnız PC'de).

  python3 tools/orun_parola_sifirla.py          # kullanıcı adı ve e-posta (salt okunur)
  python3 tools/orun_parola_sifirla.py --yeni   # yeni parola (iki kez, gizli yazılır)

Hesap defteri çalışan orun-sunucu'nun --state-dir klasöründen bulunur
(owner_accounts.sqlite). Parola komut satırından, ortamdan ya da dosyadan
alınmaz; yalnız terminalde gizli sorulur ve yalnız salt + scrypt özeti saklanır.
Yeni parola eski oturumları geçersiz kılar (password_version artar) ve yanlış
deneme kilidini kaldırır. Hesap, anahtar ve cihazlara başka dokunulmaz.
"""
from __future__ import annotations

import argparse
from contextlib import closing
import getpass
import os
from pathlib import Path
import secrets
import sqlite3
import sys

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

from backend.account_login import AccountError, _hash_password, _password  # noqa: E402


def state_dir() -> Path:
    import orun_pc_gecis as g
    try:
        live = g.live_argv(g.SUNUCU, "orun_sunucu.py")
        argv, cwd = live if live is not None else (g.exec_argv(g._show(g.SUNUCU)), Path.home())
    except Exception as exc:  # noqa: BLE001
        raise SystemExit(f"orun-sunucu bulunamadı ({exc}); --state-dir ile ver.")
    s = g.options(argv).get("state-dir")
    if not isinstance(s, str):
        raise SystemExit("orun-sunucu'da --state-dir yok; --state-dir ile ver.")
    p = Path(os.path.expanduser(s))
    return p if p.is_absolute() else cwd / p


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="ORUN hesap kullanıcı adı / parola yenileme")
    ap.add_argument("--yeni", action="store_true", help="yeni parola belirle")
    ap.add_argument("--state-dir", type=Path)
    a = ap.parse_args(argv)
    db_path = (a.state_dir or state_dir()) / "owner_accounts.sqlite"
    if not db_path.is_file():
        print(f"Hesap defteri yok: {db_path}")
        return 1
    with closing(sqlite3.connect(str(db_path), timeout=5)) as db:
        cols = {r[1] for r in db.execute("PRAGMA table_info(account_users)")}
        email = "email" if "email" in cols else "NULL"
        users = db.execute(f"SELECT site_id, actor_id, username, {email}, disabled "
                           "FROM account_users ORDER BY username").fetchall()
    if not users:
        print("Hesap defterinde kullanıcı yok.")
        return 1
    for site, actor, user, mail, disabled in users:
        print(f"Kullanıcı adı: {user}" + (f" · e-posta: {mail}" if mail else "")
              + (" · KAPALI" if disabled else ""))
    if not a.yeni:
        print("\nParolayı yenilemek için: --yeni")
        return 0
    if len(users) != 1:
        print("Birden çok kullanıcı var; bu araç yalnız tek kullanıcılı hesapta parola yeniler.")
        return 1
    site, actor, user = users[0][0], users[0][1], users[0][2]
    try:
        pw = getpass.getpass(f"'{user}' için yeni parola (en az 12 karakter, gizli): ")
        if getpass.getpass("Yeni parolayı tekrar yaz (gizli): ") != pw:
            print("İki parola aynı değil; hiçbir şey değişmedi.")
            return 1
        _password(pw)
    except AccountError:
        print("Parola 12–128 karakter olmalı; hiçbir şey değişmedi.")
        return 1
    except (EOFError, KeyboardInterrupt):
        print("\nVazgeçildi; hiçbir şey değişmedi.")
        return 1
    salt = secrets.token_bytes(16)
    verifier = _hash_password(pw, salt)
    with closing(sqlite3.connect(str(db_path), timeout=5)) as db:
        db.execute("BEGIN IMMEDIATE")
        n = db.execute("UPDATE account_users SET salt=?, verifier=?, "
                       "password_version=password_version+1 WHERE site_id=? AND actor_id=?",
                       (salt, verifier, site, actor)).rowcount
        if n != 1:
            db.execute("ROLLBACK")
            print("Kullanıcı güncellenemedi; hiçbir şey değişmedi.")
            return 1
        db.execute("DELETE FROM account_password_attempts WHERE username=?", (user,))
        db.execute("COMMIT")
    print(f"TAMAM: '{user}' için yeni parola kaydedildi. Telefonda bu kullanıcı adı ve yeni parolayla giriş yap.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
