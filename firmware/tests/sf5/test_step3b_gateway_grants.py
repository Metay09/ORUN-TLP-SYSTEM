#!/usr/bin/env python3
"""Step 3B: backend-signed, persistent Gateway grant snapshot (host only).

- OGK1 builder/signer (backend/orun_credentials.py): exact layout, Ed25519
  signature over the 1824 canonical bytes, slot keys equal the step 3A
  scoped custody ACK keys, limits; USB upload lines fit the bench buffer.
- Registry: one issuer key (created once), per-Gateway snapshot generations
  only grow and never fall below what the Gateway reports.
- Tool: `issuer` writes the PUBLIC key file; `gateway` refuses a Gateway
  built without / with another issuer key, a foreign region, a busy writer;
  uploads, waits for the reboot and checks the new generation.
- Build pre-script: pins the public key from the file, or pins nothing.
- Source contract: no forbidden class names in main.cpp; the verifier and
  installer exist only in the bench Gateway image; the writer denies the
  boot snapshot before its first flash mutation.

`--vectors FILE` writes the deterministic snapshots that
test_step3b_gateway_enrollment.cpp replays through the firmware installer.
"""
from __future__ import annotations

import os
from pathlib import Path
import stat
import sys
import tempfile
import zlib

from cryptography.hazmat.primitives.asymmetric.ed25519 import (Ed25519PrivateKey,
                                                               Ed25519PublicKey)

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "backend"))
import orun_credentials as oc  # noqa: E402
import orun_provision as op  # noqa: E402

GATEWAY = 0x09A462BD4B275BA5
OTHER_GATEWAY = 0x0102030405060708
A = oc.Credential(0x0E8ADE7E71531AA3, bytes(range(0x10, 0x20)), 1, bytes(range(0x40, 0x60)), False)
B = oc.Credential(0x1122334455667788, bytes(range(0x80, 0x90)), 3, bytes(range(0xA0, 0xC0)), False)
ISSUER = Ed25519PrivateKey.from_private_bytes(bytes(range(0x20, 0x40)))
OTHER_ISSUER = Ed25519PrivateKey.from_private_bytes(bytes(range(0x60, 0x80)))
A_KEY = "91AE7D108998E87565977EA606FE6A13F6493DEAAF5C46C791BB02B6594A0D60"  # step 3A vector


def snapshots() -> dict[str, tuple[int, int, list, Ed25519PrivateKey]]:
    return {
        "g1": (GATEWAY, 1, [(A, 0, 1)], ISSUER),
        "g2": (GATEWAY, 2, [(A, 0, 1), (B, 5, 2)], ISSUER),
        "g3_revoke": (GATEWAY, 3, [], ISSUER),
        "g5": (GATEWAY, 5, [(B, 5, 2), (A, 0, 1)], ISSUER),
        "wrong_gateway": (OTHER_GATEWAY, 9, [(A, 0, 1)], ISSUER),
        "wrong_issuer": (GATEWAY, 9, [(A, 0, 1)], OTHER_ISSUER),
    }


def write_vectors(path: Path) -> None:
    out = [f"issuer {oc.issuer_public(ISSUER).hex().upper()}",
           f"other_issuer {oc.issuer_public(OTHER_ISSUER).hex().upper()}",
           f"gateway {GATEWAY:016X}"]
    for name, (gw, gen, grants, key) in snapshots().items():
        out.append(f"snap {name} {gen:016X}")
        out += oc.ogk1_upload_lines(oc.build_ogk1(gw, gen, grants, key))
        for cred, floor, ggen in grants:
            k = oc.custody_ack_key(cred, gw, floor, ggen).hex().upper()
            out.append(f"grant {name} {cred.device_id:016X} {cred.key_epoch:08X} "
                       f"{floor:08X} {ggen:08X} {k}")
    path.write_text("\n".join(out) + "\n")


def expect_error(fn, exc=Exception, text: str | None = None):
    try:
        fn()
    except exc as e:  # noqa: PERF203
        assert text is None or text in str(e), str(e)
        return e
    raise AssertionError("no error")


def test_build() -> None:
    image = oc.build_ogk1(GATEWAY, 2, [(B, 5, 2), (A, 0, 1)], ISSUER)
    assert len(image) == oc.OGK1_STORED == 1896
    assert image[:8] == bytes.fromhex("4F474B3101200000")
    assert int.from_bytes(image[8:16], "big") == GATEWAY
    assert int.from_bytes(image[16:24], "big") == 2
    assert image[24:32] == bytes.fromhex("0000000100000000")
    # Slots sorted by Tracker id; the scoped key is the step 3A key.
    s0, s1 = image[32:88], image[88:144]
    assert s0[:8] == A.device_id.to_bytes(8, "big") and s0[24:].hex().upper() == A_KEY
    assert s0[8:24] == bytes.fromhex("00000001" "00000000" "00000001" "00000001")
    assert s1[:8] == B.device_id.to_bytes(8, "big")
    assert s1[8:24] == bytes.fromhex("00000003" "00000005" "00000002" "00000001")
    assert image[144:1824] == bytes(1680)
    Ed25519PublicKey.from_public_bytes(oc.issuer_public(ISSUER)).verify(
        image[1824:1888], image[:1824])
    assert int.from_bytes(image[1888:1892], "big") == zlib.crc32(image[:1888])
    assert image[1892:] == bytes(4)
    # Deterministic (Ed25519): the firmware test replays these exact bytes.
    assert image == oc.build_ogk1(GATEWAY, 2, [(A, 0, 1), (B, 5, 2)], ISSUER)

    full = [(oc.Credential(i + 1, bytes(16), 1, bytes(32), False), 0, 1) for i in range(32)]
    oc.build_ogk1(GATEWAY, 1, full, ISSUER)
    extra = full + [(oc.Credential(99, bytes(16), 1, bytes(32), False), 0, 1)]
    expect_error(lambda: oc.build_ogk1(GATEWAY, 1, extra, ISSUER), oc.RegistryError)
    expect_error(lambda: oc.build_ogk1(GATEWAY, 1, [(A, 0, 1), (A, 0, 1)], ISSUER), oc.RegistryError)
    me = oc.Credential(GATEWAY, bytes(16), 1, bytes(32), False)
    expect_error(lambda: oc.build_ogk1(GATEWAY, 1, [(me, 0, 1)], ISSUER), oc.RegistryError)
    expect_error(lambda: oc.build_ogk1(GATEWAY, 0, [], ISSUER), oc.RegistryError)
    expect_error(lambda: oc.build_ogk1(GATEWAY, 1, [(A, 0, 0)], ISSUER), oc.RegistryError)
    expect_error(lambda: oc.build_ogk1(0, 1, [], ISSUER), oc.RegistryError)

    lines = oc.ogk1_upload_lines(image)
    assert len(lines) == 32 and lines[0] == "BENCH OGK1 BEGIN"
    assert max(map(len, lines)) < 160               # main.cpp role_command[160]
    assert all(line == line.upper() for line in lines)
    assert lines[-1] == f"BENCH OGK1 COMMIT {zlib.crc32(image):08X}"
    assert b"".join(bytes.fromhex(line.split()[4]) for line in lines[1:-1]) == image


def test_registry(tmp: Path) -> None:
    reg = oc.CredentialRegistry(tmp / "r.sqlite")
    assert reg.issuer() is None
    expect_error(lambda: reg.gateway_snapshot(GATEWAY), oc.RegistryError, "issuer")
    k = reg.issuer(create=True)
    assert oc.issuer_public(reg.issuer(create=True)) == oc.issuer_public(k)  # never replaced
    assert oc.issuer_public(oc.CredentialRegistry(tmp / "r.sqlite").issuer()) == oc.issuer_public(k)
    assert reg.next_snapshot_generation(GATEWAY) == 1
    assert reg.next_snapshot_generation(GATEWAY) == 2
    assert reg.next_snapshot_generation(GATEWAY, at_least=7) == 8   # Gateway is ahead
    assert reg.next_snapshot_generation(GATEWAY, at_least=3) == 9   # never goes back
    assert reg.next_snapshot_generation(OTHER_GATEWAY) == 1
    a = reg.create(A.device_id)
    reg.record_grant(A.device_id, a.key_epoch, GATEWAY, 0, 1)
    gen, image = reg.gateway_snapshot(GATEWAY)
    assert gen == 10 and int.from_bytes(image[16:24], "big") == 10
    assert image[32:40] == A.device_id.to_bytes(8, "big")
    assert image[56:88] == oc.custody_ack_key(a, GATEWAY, 0, 1)
    # A replaced (retired) credential leaves the next snapshot.
    reg.create(A.device_id, replace=True)
    _, image = reg.gateway_snapshot(GATEWAY)
    assert image[32:1824] == bytes(1792)
    assert stat.S_IMODE(os.stat(tmp / "r.sqlite").st_mode) == 0o600


class FakeGateway:
    """Scripted bench Gateway: checks the upload like the firmware does."""

    def __init__(self, issuer_pub: bytes, status="EMPTY", generation=0, prefix=None):
        self.pub = issuer_pub
        self.status, self.generation = status, generation
        self.prefix = prefix or issuer_pub[:4].hex().upper()
        self.sent: list[str] = []
        self.pending: list[str] = []
        self.image = b""
        self.installed = b""
        self.reboots = 0
        self.fail_write = False

    def report(self) -> list[str]:
        return [f"BENCH GATEWAY id={GATEWAY:016X} custody_region=READY custody_ready=yes",
                "BENCH GATEWAY custody_started=0 duplicates=0 denied=0 ack_tx=0",
                f"BENCH OGK1 status={self.status} generation={self.generation:016X} "
                f"issuer={self.prefix} installer=IDLE signed_acks=0"]

    def respond(self, line: str) -> list[str]:
        if line == "BENCH?":
            return self.report()
        if line == "BENCH OGK1 BEGIN":
            self.image = b""
            return ["BENCH OGK1 OK received=0"]
        if line.startswith("BENCH OGK1 DATA "):
            _, _, _, off, data = line.split()
            assert int(off, 16) == len(self.image)
            self.image += bytes.fromhex(data)
            return [f"BENCH OGK1 OK received={len(self.image)}"]
        if line.startswith("BENCH OGK1 COMMIT "):
            if zlib.crc32(self.image) != int(line.split()[3], 16):
                return ["BENCH OGK1 FAIL reason=bad_crc"]
            if int.from_bytes(self.image[8:16], "big") != GATEWAY:
                return ["BENCH OGK1 FAIL reason=wrong_gateway"]
            try:
                Ed25519PublicKey.from_public_bytes(self.pub).verify(
                    self.image[1824:1888], self.image[:1824])
            except Exception:
                return ["BENCH OGK1 FAIL reason=not_authentic"]
            gen = int.from_bytes(self.image[16:24], "big")
            if gen <= self.generation:
                return ["BENCH OGK1 FAIL reason=not_newer"]
            out = [f"BENCH OGK1 VERIFIED generation={gen:016X}; writing page 0"]
            if self.fail_write:
                return out + ["BENCH OGK1 COMMIT FAIL reason=flash; ..."]
            self.installed, self.next_generation = self.image, gen
            return out + [f"BENCH OGK1 COMMIT PASS generation={gen:016X} page=0; rebooting to use it"]
        return []

    # Link interface
    def send(self, line: str) -> None:
        self.sent.append(line)
        self.pending += self.respond(line)

    def lines(self, seconds: float):
        while self.pending:
            yield self.pending.pop(0)

    def reconnect(self, seconds: float = 25.0) -> None:
        self.reboots += 1
        self.status, self.generation = "READY", self.next_generation


def test_tool(tmp: Path) -> None:
    quiet = lambda *_: None
    reg = oc.CredentialRegistry(tmp / "t.sqlite")
    pubfile = tmp / "orun-issuer.pub"
    pub = op.write_issuer_public(reg, pubfile, quiet)
    assert pubfile.read_text().strip() == pub.hex().upper()
    assert op.write_issuer_public(reg, pubfile, quiet) == pub          # same key on re-run
    assert "PRIVATE" not in pubfile.read_text() and len(pubfile.read_text().strip()) == 64

    a = reg.create(A.device_id)
    expect_error(lambda: op.install_gateway_snapshot(FakeGateway(pub), reg, quiet),
                 op.ProvisionError, "kayıtlı yetki yok")
    reg.record_grant(A.device_id, a.key_epoch, GATEWAY, 0, 1)

    dev = FakeGateway(pub)
    assert op.install_gateway_snapshot(dev, reg, quiet) == 1
    assert dev.reboots == 1 and dev.status == "READY" and dev.generation == 1
    assert dev.installed[56:88] == oc.custody_ack_key(a, GATEWAY, 0, 1)
    # Next run: a higher generation, the device's own active one respected.
    dev2 = FakeGateway(pub, status="READY", generation=41)
    op.install_gateway_snapshot(dev2, reg, quiet)
    assert dev2.generation == 42

    for status, text in (("NO_ISSUER_KEY", "issuer anahtarı yok"),
                         ("REJECTED", "CLEAN"),
                         ("DENIED_UNTIL_REBOOT", "yeniden başlatın"),
                         ("UNAVAILABLE", "hazır değil")):
        d = FakeGateway(pub, status=status)
        expect_error(lambda: op.install_gateway_snapshot(d, reg, quiet), op.ProvisionError, text)
        assert not any(s.startswith("BENCH OGK1") for s in d.sent)
    d = FakeGateway(pub, prefix="DEADBEEF")
    expect_error(lambda: op.install_gateway_snapshot(d, reg, quiet), op.ProvisionError, "başka bir issuer")
    assert not any(s.startswith("BENCH OGK1") for s in d.sent)
    d = FakeGateway(oc.issuer_public(OTHER_ISSUER), prefix=pub[:4].hex().upper())
    expect_error(lambda: op.install_gateway_snapshot(d, reg, quiet), op.ProvisionError, "imza")
    d = FakeGateway(pub)
    d.fail_write = True
    expect_error(lambda: op.install_gateway_snapshot(d, reg, quiet), op.ProvisionError, "yazamadı")
    old = FakeGateway(pub)
    old.report = lambda: [f"BENCH GATEWAY id={GATEWAY:016X} custody_region=READY"]
    expect_error(lambda: op.install_gateway_snapshot(old, reg, quiet), op.ProvisionError, "3B'den eski")
    # The CLI: issuer subcommand writes the given file.
    other = tmp / "x.pub"
    assert op.main(["--registry", str(tmp / "t.sqlite"), "issuer", "--public-key-file", str(other)]) == 0
    assert other.read_text().strip() == pub.hex().upper()


def run_prescript(build: Path, keyfile: Path | None) -> str:
    class Env:
        def __init__(self):
            self.cpppath = []

        def subst(self, s):
            assert s == "$BUILD_DIR"
            return str(build)

        def Append(self, CPPPATH):  # noqa: N802,N803
            self.cpppath += CPPPATH

    env = Env()
    saved = os.environ.get("ORUN_ISSUER_PUBLIC_KEY_FILE")
    os.environ["ORUN_ISSUER_PUBLIC_KEY_FILE"] = str(keyfile or build / "missing.pub")
    try:
        code = (ROOT / "firmware/scripts/step3b_issuer_key.py").read_text()
        exec(compile(code, "step3b_issuer_key.py", "exec"),
             {"Import": lambda _name: None, "env": env, "__name__": "prescript"})
    finally:
        if saved is None:
            del os.environ["ORUN_ISSUER_PUBLIC_KEY_FILE"]
        else:
            os.environ["ORUN_ISSUER_PUBLIC_KEY_FILE"] = saved
    assert env.cpppath == [str(build / "step3b_generated")]
    return (build / "step3b_generated" / "orun_step3b_issuer_key.h").read_text()


def test_prescript(tmp: Path) -> None:
    import contextlib
    import io
    with contextlib.redirect_stdout(io.StringIO()):
        h = run_prescript(tmp / "b1", None)
        assert "#define ORUN_STEP3B_ISSUER_PINNED 0" in h and "PUBLIC_KEY" not in h
        key = tmp / "k.pub"
        key.write_text(oc.issuer_public(ISSUER).hex().upper() + "\n")
        h = run_prescript(tmp / "b2", key)
        assert "#define ORUN_STEP3B_ISSUER_PINNED 1" in h
        first = oc.issuer_public(ISSUER)
        assert "{ " + ", ".join(f"0x{b:02X}" for b in first) + " }" in h
        key.write_text("00" * 32)
        expect_error(lambda: run_prescript(tmp / "b3", key), SystemExit)
        key.write_text("not a key")
        expect_error(lambda: run_prescript(tmp / "b4", key), SystemExit)


def test_source_contract() -> None:
    main = (ROOT / "firmware/src/main.cpp").read_text()
    for name in ("GatewayEd25519Cc310Verifier", "GatewayGrantSnapshotReader",
                 "GatewayProductStartupGuard", "Step3bGrantInstaller"):
        assert name not in main, name
    ini = (ROOT / "firmware/platformio.ini").read_text()
    assert ini.count("-DORUN_STEP3B_GATEWAY_ENROLLMENT") == 1
    gw_env = ini.split("[env:rak4630_sf5l0_gateway_bench]")[1].split("\n[")[0]
    assert "-DORUN_STEP3B_GATEWAY_ENROLLMENT" in gw_env
    assert "pre:scripts/step3b_issuer_key.py" in gw_env
    assert "-DORUN_SF5L0_GATEWAY" in gw_env
    assert ini.count("-DORUN_SF5K3AA_TARGET_CRYPTO_PROBE=1") == 1
    assert '#error "Step 3B grant snapshots exist only' in main
    # The Tracker SecurityStore is never mounted on the Gateway's OGK1 pages.
    sec = main.split("#elif defined(ORUN_STEP3B_GATEWAY_ENROLLMENT)")[1].split("#else")[0]
    assert "security_store.begin" not in sec
    inst = (ROOT / "firmware/src/step3b_gateway_enrollment.cpp").read_text()
    commit = inst.split("Step3bGrantInstaller::commit(")[1].split("Step3bGrantInstaller::poll()")[0]
    assert "erasePage(" not in commit and "program(" not in commit
    assert commit.index("current.denyForCurrentBoot();") > commit.index("candidate->begin(")
    assert "state_ = State::kWriting;" in commit.split("current.denyForCurrentBoot();")[1]
    assert "flash_.pollPending()" in inst and "secureWipe(image_" in inst
    for mutator in ("sd_flash_", "Radio.Send(", "commitCredential("):
        assert mutator not in inst
    hdr = (ROOT / "firmware/include/step3b_gateway_enrollment.h").read_text()
    assert "#if defined(ORUN_STEP3B_GATEWAY_ENROLLMENT)" in hdr
    gw = (ROOT / "firmware/src/sf5l0_bench_gateway.cpp").read_text()
    assert "signed_grants->begin(gateway_id, true, false)" in gw
    assert "grants->setSigned(signed_grants)" in gw
    assert "ORUN_STEP3B_ISSUER_PINNED" in gw
    assert "kIssuer[32] = ORUN_STEP3B_ISSUER_PUBLIC_KEY" in gw
    suite = (ROOT / "firmware/tests/run_host_tests.sh").read_text()
    assert "run_step3b_gateway_grants_host.sh" in suite


def main() -> int:
    if len(sys.argv) == 3 and sys.argv[1] == "--vectors":
        write_vectors(Path(sys.argv[2]))
        return 0
    test_build()
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        test_registry(tmp)
        test_tool(tmp)
        test_prescript(tmp)
    test_source_contract()
    print("Step 3B signed Gateway grants (backend, tool, build pin, source): PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
