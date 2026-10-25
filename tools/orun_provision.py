#!/usr/bin/env python3
"""Step 3A: give a Tracker its own real key, and its Gateway the matching
custody ACK grant, over USB, from the backend credential registry.

  new      create a random credential for a device in the registry
  tracker  write the Tracker's credential over USB and grant its Gateway;
           record the grant scope (generation/floor) the Tracker reports
  gateway  write every recorded grant for this Gateway over USB (RAM only on
           the bench Gateway: run again after the Gateway restarts)
  list     show devices, epochs and grants (never keys)

The root key leaves the registry only towards the Tracker it belongs to, on
the USB cable (physical access = authorization, ADR M7P6 §11). The Gateway
gets only the scoped custody ACK key. Nothing secret is printed.

A Tracker that already holds the public test key must first be cleaned with
the rak4630_m7p6f_security_cleanup image ("CLEAN ALL"), then reflashed with
the Tracker bench image.

Examples (registry default: ~/orun-credentials.sqlite):
  python3 tools/orun_provision.py tracker --port /dev/serial/by-id/...0E8ADE7E71531AA3-if00 \\
      --gateway 09A462BD4B275BA5 --create
  python3 tools/orun_provision.py gateway --port /dev/serial/by-id/...09A462BD4B275BA5-if00
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys
import time
from typing import Callable, Iterable, Optional

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "backend"))
import orun_credentials as oc  # noqa: E402

DEFAULT_REGISTRY = Path.home() / "orun-credentials.sqlite"

TRACKER_ID = re.compile(r"BENCH TRACKER id=([0-9A-F]{16}) ")
CREDENTIAL = re.compile(
    r"BENCH CREDENTIAL credential=([0-9A-F]{32}|none) epoch=([0-9A-F]{8}) "
    r"grant_generation=([0-9A-F]{8}) custody_floor=([0-9A-F]{8})")
GRANTED = re.compile(r"granted_gateway=([0-9A-F]{16}|none)")
GATEWAY_ID = re.compile(r"BENCH GATEWAY id=([0-9A-F]{16}) ")
SETUP_DONE = "BENCH SETUP PASS"
SETUP_FAIL = re.compile(r"BENCH (?:SETUP|PROVISION) (?:FAIL|rejected)[^\n]*")
GRANT_REPLY = re.compile(r"BENCH GRANT (OK added|OK replaced|FAIL \w+|rejected)[^\n]*")


class ProvisionError(Exception):
    pass


class Link:
    """Line-oriented serial link (pyserial), replaceable in tests."""

    def __init__(self, port: str):
        try:
            import serial  # pyserial
        except ImportError:
            raise SystemExit("pyserial yok: python3 -m pip install pyserial")
        self.ser = serial.Serial(port, 115200, timeout=0.2)
        time.sleep(0.5)
        self.buf = b""

    def send(self, line: str) -> None:
        self.ser.write((line + "\n").encode("ascii"))

    def lines(self, seconds: float) -> Iterable[str]:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.buf += self.ser.read(512)
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                yield raw.decode("utf-8", "replace").rstrip("\r")


def wait_for(link, match: Callable[[str], Optional[object]], seconds: float):
    for line in link.lines(seconds):
        got = match(line)
        if got:
            return got
    return None


def tracker_report(link) -> dict:
    """BENCH? on the Tracker -> id, credential id, epoch, grant scope."""
    link.send("BENCH?")
    out: dict = {}
    for line in link.lines(5.0):
        m = CREDENTIAL.search(line)
        if m:
            out.update(credential=m[1], epoch=int(m[2], 16),
                       generation=int(m[3], 16), floor=int(m[4], 16))
        m = TRACKER_ID.search(line)
        if m:
            out["id"] = int(m[1], 16)
            g = GRANTED.search(line)
            out["granted"] = None if not g or g[1] == "none" else int(g[1], 16)
            if "credential" in out:
                return out
    if "id" not in out:
        raise ProvisionError("Tracker bench image did not answer BENCH? (doğru port mu?)")
    return out


def provision_tracker(link, registry: oc.CredentialRegistry, gateway: int,
                      create: bool, log=print) -> dict:
    rep = tracker_report(link)
    dev = rep["id"]
    cred = registry.active(dev)
    if cred is None:
        if not create:
            raise ProvisionError(f"{dev:016X} için kayıtlı anahtar yok (--create ekle)")
        cred = registry.create(dev)
        log(f"[kayıt] {dev:016X} için yeni anahtar oluşturuldu (epoch {cred.key_epoch})")
    if rep.get("credential") not in (None, "none") and rep["credential"] != cred.credential_id.hex().upper():
        raise ProvisionError(
            "tasmada başka bir anahtar var (ör. test anahtarı); önce temizleme imajıyla "
            "CLEAN ALL, sonra tasma imajı yüklenmeli")
    link.send(oc.provision_line(cred, gateway))
    def done(line: str):
        if SETUP_DONE in line:
            return "ok"
        m = SETUP_FAIL.search(line)
        return m[0] if m else None
    r = wait_for(link, done, 60.0)
    if r != "ok":
        raise ProvisionError(f"tasma kurulumu başarısız: {r or 'cevap yok (60 sn)'}")
    rep = tracker_report(link)
    if rep.get("credential") != cred.credential_id.hex().upper() or rep.get("epoch") != cred.key_epoch:
        raise ProvisionError("tasma raporu kayıttaki anahtarla eşleşmiyor")
    if rep.get("granted") != gateway:
        raise ProvisionError("tasma gateway'i yetkilendirmedi")
    registry.record_grant(dev, cred.key_epoch, gateway, rep["floor"], rep["generation"])
    log(f"[tasma] {dev:016X} kuruldu: epoch {cred.key_epoch}, gateway {gateway:016X} "
        f"(generation {rep['generation']}, floor {rep['floor']})")
    return rep


def grant_gateway(link, registry: oc.CredentialRegistry, log=print) -> int:
    link.send("BENCH?")
    gw = wait_for(link, lambda line: (m := GATEWAY_ID.search(line)) and int(m[1], 16), 5.0)
    if not gw:
        raise ProvisionError("Gateway bench image did not answer BENCH? (doğru port mu?)")
    grants = registry.grants_for_gateway(gw)
    if not grants:
        raise ProvisionError(f"gateway {gw:016X} için kayıtlı yetki yok (önce 'tracker' adımı)")
    for cred, floor, generation in grants:
        link.send(oc.grant_line(cred, gw, floor, generation))
        r = wait_for(link, lambda line: (m := GRANT_REPLY.search(line)) and m[0], 5.0)
        if not r or not r.startswith("BENCH GRANT OK"):
            raise ProvisionError(f"gateway yetkiyi almadı: {r or 'cevap yok'}")
        log(f"[gateway] {gw:016X}: tasma {cred.device_id:016X} epoch {cred.key_epoch} yetkisi yazıldı")
    return len(grants)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--registry", default=str(DEFAULT_REGISTRY))
    sub = ap.add_subparsers(dest="cmd", required=True)
    n = sub.add_parser("new")
    n.add_argument("--device", required=True)
    n.add_argument("--replace", action="store_true", help="retire the active credential")
    t = sub.add_parser("tracker")
    t.add_argument("--port", required=True)
    t.add_argument("--gateway", required=True)
    t.add_argument("--create", action="store_true", help="create a credential if none")
    g = sub.add_parser("gateway")
    g.add_argument("--port", required=True)
    sub.add_parser("list")
    args = ap.parse_args(argv)
    reg = oc.CredentialRegistry(args.registry)
    try:
        if args.cmd == "new":
            c = reg.create(int(args.device, 16), replace=args.replace)
            print(f"[kayıt] {c.device_id:016X} epoch {c.key_epoch} credential "
                  f"{c.credential_id.hex().upper()[:8]}…")
        elif args.cmd == "tracker":
            provision_tracker(Link(args.port), reg, int(args.gateway, 16), args.create)
        elif args.cmd == "gateway":
            grant_gateway(Link(args.port), reg)
        else:
            for c in reg.all():
                print(f"{c.device_id:016X} epoch={c.key_epoch} "
                      f"credential={c.credential_id.hex().upper()[:8]}… "
                      f"{'eski' if c.retired else 'etkin'}")
    except (ProvisionError, oc.RegistryError) as e:
        print(f"HATA: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
