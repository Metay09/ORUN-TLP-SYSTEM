#!/usr/bin/env python3
import hashlib
import io
import subprocess
import tarfile
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[3]
script = root / "firmware/tests/codeql/verify_bundle_install.py"


def make_bundle(path, payload):
    with tarfile.open(path, mode="w:gz") as archive:
        info = tarfile.TarInfo("codeql/codeql")
        info.size = len(payload)
        info.mode = 0o755
        archive.addfile(info, io.BytesIO(payload))


def bundle_sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


with tempfile.TemporaryDirectory(prefix="orun-codeql-integrity-") as td:
    td = Path(td)
    archive = td / "codeql-bundle-linux64.tar.gz"
    binary = td / "codeql"
    payload = b"trusted-codeql-executable-fixture\n"

    make_bundle(archive, payload)
    expected_archive_sha256 = bundle_sha256(archive)
    binary.write_bytes(payload)

    good = subprocess.run(
        [
            "python3",
            str(script),
            str(archive),
            str(binary),
            expected_archive_sha256,
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    assert good.returncode == 0, (good.stdout, good.stderr)
    assert "integrity: PASS" in good.stdout

    binary.write_bytes(payload + b"tampered")
    tampered_binary = subprocess.run(
        [
            "python3",
            str(script),
            str(archive),
            str(binary),
            expected_archive_sha256,
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    assert tampered_binary.returncode == 2
    assert "differs from checksum-verified bundle" in tampered_binary.stderr

    binary.write_bytes(payload)
    archive.write_bytes(archive.read_bytes() + b"tampered")
    tampered_archive = subprocess.run(
        [
            "python3",
            str(script),
            str(archive),
            str(binary),
            expected_archive_sha256,
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    assert tampered_archive.returncode == 2
    assert "bundle archive checksum mismatch" in tampered_archive.stderr

print("DEVQ1 CodeQL extracted-bundle integrity checks: PASS")
