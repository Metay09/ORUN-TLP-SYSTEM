#!/usr/bin/env python3
"""Step 3A/3B: give a Tracker its own real key, and its Gateway the matching
custody ACK grant, over USB, from the backend credential registry.

  issuer   create (once) the Gateway issuer key; write its PUBLIC key to
           ~/orun-issuer.pub, which the Gateway bench build pins
  new      create a random credential for a device in the registry
  tracker  write the Tracker's credential over USB and grant its Gateway;
           record the grant scope (generation/floor) the Tracker reports
  gateway  sign this Gateway's grant snapshot (OGK1: every recorded grant)
           and install it over USB; the Gateway checks the signature, its id
           and a higher generation, stores it in flash and reboots (step 3B).
           --ram: the step 3A RAM-only grants instead (lost on restart)
  list     show devices, epochs and grants (never keys)

The root key leaves the registry only towards the Tracker it belongs to, on
the USB cable (physical access = authorization, ADR M7P6 §11). The Gateway
gets only the scoped custody ACK keys, signed by the issuer. Nothing secret
is printed.

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
import os
from pathlib import Path
import re
import sys
import time
from typing import Callable, Iterable, Optional

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "backend"))
import orun_credentials as oc  # noqa: E402

DEFAULT_REGISTRY = Path.home() / "orun-credentials.sqlite"
DEFAULT_ISSUER_PUB = Path.home() / "orun-issuer.pub"   # firmware/scripts/step3b_issuer_key.py

TRACKER_ID = re.compile(r"BENCH TRACKER id=([0-9A-F]{16}) ")
CREDENTIAL = re.compile(
    r"BENCH CREDENTIAL credential=([0-9A-F]{32}|none) epoch=([0-9A-F]{8}) "
    r"grant_generation=([0-9A-F]{8}) custody_floor=([0-9A-F]{8})")
GRANTED = re.compile(r"granted_gateway=([0-9A-F]{16}|none)")
GATEWAY_ID = re.compile(r"BENCH GATEWAY id=([0-9A-F]{16}) ")
SETUP_DONE = "BENCH SETUP PASS"
SETUP_FAIL = re.compile(r"BENCH (?:SETUP|PROVISION) (?:FAIL|rejected)[^\n]*")
GRANT_REPLY = re.compile(r"BENCH GRANT (OK added|OK replaced|FAIL \w+|rejected)[^\n]*")
OGK1_STATUS = re.compile(
    r"BENCH OGK1 status=(\w+) generation=([0-9A-F]{16}) issuer=([0-9A-F]{8}) installer=(\w+)")
OGK1_REPLY = re.compile(r"BENCH OGK1 (OK|VERIFIED|COMMIT PASS|COMMIT FAIL|FAIL)\b[^\n]*")
OGK1_REASON = re.compile(r"reason=(\w+)")

# What the Gateway's refusal means, for the owner (Turkish UI text).
OGK1_REASONS = {
    "not_newer": "gateway'de daha yeni (veya aynı) bir yetki paketi var",
    "wrong_gateway": "paket başka bir gateway için",
    "not_authentic": "imza/biçim doğrulanamadı (gateway başka bir issuer anahtarıyla derlenmiş olabilir)",
    "region_not_ready": "gateway'in güvenlik bölgesi okunamıyor veya başka veri tutuyor: temizleme imajıyla CLEAN gerekli",
    "bad_crc": "aktarım hatası (CRC); tekrar deneyin",
    "busy_reboot_first": "gateway bir önceki yazmayı bitirdi/bozuldu: gateway'i yeniden başlatın",
    "no_issuer_key": "gateway imajında issuer anahtarı yok: ~/orun-issuer.pub ile yeniden derleyip yükleyin",
    "NO_ISSUER_KEY": "gateway imajında issuer anahtarı yok: ~/orun-issuer.pub ile yeniden derleyip yükleyin",
    "not_in_this_image": "gateway imajı 3B'den eski: rak4630_sf5l0_gateway_bench yeniden yüklenmeli",
}


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

    def reconnect(self, seconds: float = 25.0) -> None:
        """The Gateway reboots after installing a snapshot: USB re-enumerates."""
        import serial  # pyserial
        port = self.ser.port
        try:
            self.ser.close()
        except Exception:
            pass
        time.sleep(2.0)
        end = time.monotonic() + seconds
        while True:
            try:
                self.ser = serial.Serial(port, 115200, timeout=0.2)
                break
            except (serial.SerialException, OSError):
                if time.monotonic() > end:
                    raise ProvisionError("gateway yeniden başladıktan sonra USB portu açılamadı")
                time.sleep(0.5)
        time.sleep(1.5)
        self.buf = b""

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


def write_issuer_public(registry: oc.CredentialRegistry, path: Path, log=print) -> bytes:
    """Create the issuer once; (re)write its PUBLIC key file for the build."""
    existed = registry.issuer() is not None
    key = registry.issuer(create=True)
    pub = oc.issuer_public(key)
    path.write_text(pub.hex().upper() + "\n")
    os.chmod(path, 0o644)
    log(f"[issuer] {'mevcut' if existed else 'yeni'} gateway issuer anahtarı; "
        f"açık anahtar {path} (başı {pub[:4].hex().upper()})")
    return pub


def gateway_report(link) -> tuple[int, Optional[re.Match]]:
    """BENCH? on the Gateway -> (id, OGK1 status match or None)."""
    link.send("BENCH?")
    gw, ogk1 = None, None
    for line in link.lines(5.0):
        m = GATEWAY_ID.search(line)
        if m and gw is None:
            gw = int(m[1], 16)
        m = OGK1_STATUS.search(line)
        if m:
            ogk1 = m
            if gw is not None:
                break
    if gw is None:
        raise ProvisionError("Gateway bench image did not answer BENCH? (doğru port mu?)")
    return gw, ogk1


def _ogk1_reply(link, seconds: float) -> str:
    r = wait_for(link, lambda line: (m := OGK1_REPLY.search(line)) and m[0], seconds)
    if not r:
        raise ProvisionError("gateway cevap vermedi (OGK1)")
    return r


def _ogk1_fail(reply: str) -> ProvisionError:
    m = OGK1_REASON.search(reply)
    reason = m[1] if m else reply
    return ProvisionError(f"gateway paketi reddetti: {OGK1_REASONS.get(reason, reason)}")


def install_gateway_snapshot(link, registry: oc.CredentialRegistry, log=print) -> int:
    """Step 3B: sign every recorded grant of this Gateway and install it."""
    gw, st = gateway_report(link)
    if st is None:
        raise ProvisionError(OGK1_REASONS["not_in_this_image"])
    status, active, prefix = st[1], int(st[2], 16), st[3]
    if status in ("NO_ISSUER_KEY", "CRYPTO_INIT_FAIL"):
        raise ProvisionError(OGK1_REASONS.get(status, f"gateway issuer durumu {status}"))
    if status == "REJECTED":
        raise ProvisionError(OGK1_REASONS["region_not_ready"])
    if status == "DENIED_UNTIL_REBOOT" or st[4] in ("WRITING", "DONE", "FAULT"):
        raise ProvisionError(OGK1_REASONS["busy_reboot_first"])
    if status not in ("READY", "EMPTY"):
        raise ProvisionError(f"gateway güvenlik bölgesi hazır değil ({status})")
    issuer = registry.issuer()
    if issuer is None:
        raise ProvisionError("kayıtta issuer anahtarı yok: önce 'issuer' adımı")
    if oc.issuer_public(issuer)[:4].hex().upper() != prefix:
        raise ProvisionError("gateway başka bir issuer anahtarıyla derlenmiş "
                             f"(gateway {prefix}); ~/orun-issuer.pub ile yeniden derleyip yükleyin")
    grants = registry.grants_for_gateway(gw)
    if not grants:
        raise ProvisionError(f"gateway {gw:016X} için kayıtlı yetki yok (önce 'tracker' adımı)")
    generation, image = registry.gateway_snapshot(gw, at_least=active)
    lines = oc.ogk1_upload_lines(image)
    for line in lines[:-1]:
        link.send(line)
        r = _ogk1_reply(link, 5.0)
        if not r.startswith("BENCH OGK1 OK"):
            raise _ogk1_fail(r)
    link.send(lines[-1])
    r = _ogk1_reply(link, 10.0)
    if not r.startswith("BENCH OGK1 VERIFIED"):
        raise _ogk1_fail(r)
    r = _ogk1_reply(link, 20.0)
    if not r.startswith("BENCH OGK1 COMMIT PASS"):
        raise ProvisionError("gateway yetki paketini flash'a yazamadı; gateway'i yeniden "
                             "başlatıp tekrar deneyin")
    log(f"[gateway] {gw:016X}: imzalı yetki paketi yazıldı (generation {generation}); "
        "gateway yeniden başlıyor")
    link.reconnect()
    gw2, st2 = None, None
    for _ in range(4):  # the Gateway may still be booting
        try:
            gw2, st2 = gateway_report(link)
        except ProvisionError:
            continue
        if st2 is not None:
            break
    if gw2 != gw or st2 is None or st2[1] != "READY" or int(st2[2], 16) != generation:
        raise ProvisionError("yeniden başlayan gateway yeni yetki paketini göstermiyor "
                             f"({st2[0] if st2 else 'OGK1 satırı yok'})")
    for cred, _floor, _gen in grants:
        log(f"[gateway] {gw:016X}: tasma {cred.device_id:016X} epoch {cred.key_epoch} "
            "yetkisi kalıcı")
    return len(grants)


def grant_gateway(link, registry: oc.CredentialRegistry, log=print) -> int:
    """Step 3A interim: RAM-only grants (lost when the Gateway restarts)."""
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
    g.add_argument("--ram", action="store_true",
                   help="step 3A RAM-only grants instead of the signed snapshot")
    i = sub.add_parser("issuer")
    i.add_argument("--public-key-file", default=str(DEFAULT_ISSUER_PUB))
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
            if args.ram:
                grant_gateway(Link(args.port), reg)
            else:
                install_gateway_snapshot(Link(args.port), reg)
        elif args.cmd == "issuer":
            write_issuer_public(reg, Path(args.public_key_file).expanduser())
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
