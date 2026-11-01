#!/usr/bin/env python3
"""PC'deki üç eski servisten tek ORUN sunucusuna geçiş yardımcısı (1a).

Mevcut servislerin GERÇEK başlatma satırlarını okur (systemctl --user show),
tahmin etmez; tek sunucu için aynı veritabanı, kimlik kayıtları, hesap
klasörü, adresler ve köprü anahtarıyla bir servis dosyası yazar.
Servisleri başlatıp durdurmaz; onu tools/orun_guncelle.sh yapar.

  python3 tools/orun_pc_gecis.py write-unit RELEASE_DIR UNIT_PATH
  python3 tools/orun_pc_gecis.py describe            # çalışan orun-sunucu ayarları
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

BACKEND = "orun-backend.service"
SHARED = "orun-shared-sync.service"
SUNUCU = "orun-sunucu.service"
HOME = Path.home()


class GecisHatasi(RuntimeError):
    pass


def exec_argv(show_value: str) -> list[str]:
    """argv of the effective ExecStart from `systemctl show -p ExecStart --value`."""
    starts = re.findall(r"argv\[\]=(.*?) ; ignore_errors=", show_value)
    if len(starts) != 1:
        raise GecisHatasi("tek bir ExecStart bekleniyordu")
    return shlex.split(starts[0])


def options(argv: list[str]) -> dict[str, str | bool]:
    out: dict[str, str | bool] = {}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a.startswith("--"):
            if "=" in a:
                k, v = a[2:].split("=", 1)
                out[k] = v
            elif i + 1 < len(argv) and not argv[i + 1].startswith("--"):
                out[a[2:]] = argv[i + 1]
                i += 1
            else:
                out[a[2:]] = True
        i += 1
    return out


def backend_config(argv: list[str]) -> dict:
    o = options(argv)
    return {
        "db": str(o.get("db", HOME / "orun-backend-bench.sqlite")),
        "registry": str(o.get("registry", HOME / "orun-credentials.sqlite")),
        "host": str(o.get("host", "127.0.0.1")),
        "port": int(o.get("port", 8090)),
        "token_file": o.get("token-file"),
        "token": o.get("token"),
        "bench": bool(o.get("bench-test-key", False)),
    }


def shared_config(argv: list[str]) -> dict:
    o = options(argv)
    if not isinstance(o.get("state-dir"), str) or not isinstance(o.get("observation-site"), str):
        raise GecisHatasi("hesap servisinde --state-dir ve --observation-site bulunamadı")
    port = int(o.get("port", 0))
    if not 0 < port < 65536:
        raise GecisHatasi("hesap servisinin sabit portu bulunamadı")
    return {"state_dir": o["state-dir"], "site": o["observation-site"], "port": port}


def unit_text(release: str, python: str, b: dict, s: dict, token_file: str) -> str:
    args = [python, "-I", f"{release}/tools/orun_sunucu.py",
            "--state-dir", s["state_dir"], "--db", b["db"], "--registry", b["registry"],
            "--site", s["site"], "--listen", f"127.0.0.1:{s['port']}",
            "--bridge-listen", f"{b['host']}:{b['port']}", "--bridge-token-file", token_file]
    if b["bench"]:
        args.append("--bench-test-key")
    return ("[Unit]\nDescription=ORUN sunucusu (tek süreç)\nAfter=network-online.target\n\n"
            "[Service]\nExecStart=" + shlex.join(args) + "\nRestart=on-failure\nRestartSec=5\n\n"
            "[Install]\nWantedBy=default.target\n")


def _show(unit: str) -> str:
    r = subprocess.run(["systemctl", "--user", "show", "-p", "ExecStart", "--value", unit],
                       capture_output=True, text=True, check=False)
    if r.returncode != 0 or "argv[]=" not in r.stdout:
        raise GecisHatasi(f"{unit} okunamadı")
    return r.stdout


def write_unit(release: str, unit_path: str) -> None:
    b_argv, s_argv = exec_argv(_show(BACKEND)), exec_argv(_show(SHARED))
    b, s = backend_config(b_argv), shared_config(s_argv)
    token_file = b["token_file"]
    if not token_file:
        if not isinstance(b["token"], str) or len(b["token"]) < 8:
            raise GecisHatasi("Backend köprü anahtarı bulunamadı")
        # Anahtar zaten servis satırında açıktaydı; 0600 dosyaya taşınır.
        path = HOME / ".config" / "orun" / "bridge_token"
        path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(b["token"] + "\n")
        token_file = str(path)
    python = s_argv[0] if s_argv[0].endswith(("python3", "python")) else "/usr/bin/python3"
    target = Path(unit_path)
    if target.exists():
        raise GecisHatasi(f"{target} zaten var")
    target.write_text(unit_text(release, python, b, s, token_file))
    print(f"BRIDGE={b['host']}:{b['port']}")
    print(f"ACCOUNT_PORT={s['port']}")
    print(f"TOKEN_FILE={token_file}")


def describe() -> None:
    o = options(exec_argv(_show(SUNUCU)))
    print(f"BRIDGE={o['bridge-listen']}")
    print(f"ACCOUNT_PORT={str(o['listen']).rsplit(':', 1)[1]}")
    print(f"TOKEN_FILE={o['bridge-token-file']}")


def main(argv: list[str]) -> int:
    try:
        if argv[:1] == ["write-unit"] and len(argv) == 3:
            write_unit(argv[1], argv[2])
        elif argv == ["describe"]:
            describe()
        else:
            print(__doc__, file=sys.stderr)
            return 2
    except (GecisHatasi, KeyError, ValueError, OSError) as exc:
        print(f"HATA: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
