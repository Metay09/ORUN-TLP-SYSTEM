#!/usr/bin/env python3
"""Reversible ORUN PC Edge USB/BLE systemd switch. Operates on one user service.

Never runs a second PC Edge process, changes backend, or touches the inbox DB.
The BLE option is for the SF5L0 PUBLIC TEST KEY gateway only.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HOME = Path.home()
BASE = HOME / ".config/systemd/user"
UNIT = BASE / "orun-pc-edge.service"
DROP = BASE / "orun-pc-edge.service.d"
BENCH = DROP / "10-bench-release.conf"
BLE = DROP / "20-ble-transport.conf"
MAC = re.compile(r"(?:[0-9A-F]{2}:){5}[0-9A-F]{2}", re.ASCII)


def validate_address(address: str) -> None:
    if not MAC.fullmatch(address):
        raise ValueError("BLE address must be uppercase XX:XX:XX:XX:XX:XX")


def build_ble_start(address: str) -> str:
    if not UNIT.is_file() or not BENCH.is_file():
        raise ValueError("USB service and 10-bench-release.conf must exist")
    launch = [ln[len("ExecStart="):] for ln in BENCH.read_text().splitlines()
              if ln.startswith("ExecStart=") and ln != "ExecStart="]
    if len(launch) != 1:
        raise ValueError("Unexpected bench override format")
    args = shlex.split(launch[0])
    prefix = ["/usr/bin/python3", "-u", str(ROOT / "tools/orun_pc_edge.py")]
    names = ("--mode", "--backend-url", "--edge-id", "--token-file",
             "--port", "--bench-release-gateway")
    if args[:3] != prefix or len(args) != 3 + 2 * len(names):
        raise ValueError("Unexpected PC Edge service launch")
    values = {}
    for key, value in zip(args[3::2], args[4::2]):
        if key not in names or key in values:
            raise ValueError("Unexpected or duplicate PC Edge argument")
        values[key] = value
    if set(values) != set(names):
        raise ValueError("Incomplete PC Edge launch")
    if (values["--mode"] != "usb" or
            values["--token-file"] != str(HOME / ".config/orun/pc-edge.token") or
            not values["--port"].startswith("/dev/serial/by-id/") or
            not re.fullmatch(r"[0-9A-F]{16}", values["--bench-release-gateway"])):
        raise ValueError("Not the expected SF5L0 USB bench service")
    values["--mode"] = "ble"
    next_args = prefix[:]
    for name in ("--mode", "--backend-url", "--edge-id", "--token-file",
                 "--bench-release-gateway"):
        next_args.extend((name, values[name]))
    next_args.extend(("--ble-address", address))
    return " ".join(next_args)


def run_systemctl() -> None:
    subprocess.run(("systemctl", "--user", "daemon-reload"), check=True)
    subprocess.run(("systemctl", "--user", "restart", "orun-pc-edge.service"), check=True)
    subprocess.run(("systemctl", "--user", "is-active", "orun-pc-edge.service"), check=True)
    subprocess.run(("systemctl", "--user", "show", "orun-pc-edge.service",
                    "-p", "ExecStart", "--no-pager"), check=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mode", choices=("ble", "usb", "validate"))
    ap.add_argument("arg", nargs="?")
    ap.add_argument("extra", nargs="?")
    args = ap.parse_args()
    if args.mode == "validate":
        if args.arg != "ble" or not args.extra:
            ap.error("transport validate ble MAC")
        validate_address(args.extra)
        print("BLE address: VALID")
        return 0
    if args.mode == "ble":
        if not args.arg or args.extra:
            ap.error("transport ble MAC")
        validate_address(args.arg)
        __import__("bleak")
        if BLE.exists():
            raise ValueError("BLE mode already configured; no overwrite")
        cmd = build_ble_start(args.arg)
        DROP.mkdir(mode=0o700, parents=True, exist_ok=True)
        text = "[Service]\nExecStart=\nExecStart=" + cmd + "\n"
        # Exclusive create: preserve unrelated/manual overrides.
        fd = os.open(BLE, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
        print("PC Edge: BLE selected", args.arg, flush=True)
    elif args.mode == "usb":
        if args.arg or args.extra:
            ap.error("transport usb")
        if not BLE.exists():
            print("PC Edge: already in default USB mode")
            return 0
        data = BLE.read_text(encoding="utf-8")
        if (not data.startswith("[Service]\nExecStart=\nExecStart=") or
                " --mode ble " not in data or " --ble-address " not in data):
            raise ValueError("Unknown BLE override: will not remove")
        BLE.unlink()
        print("PC Edge: default USB selected", flush=True)
    run_systemctl()
    print("Backend service unchanged.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError, ImportError) as exc:
        print("TRANSPORT ERROR:", type(exc).__name__, str(exc), file=sys.stderr)
        raise SystemExit(1)
