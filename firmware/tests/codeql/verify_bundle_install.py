#!/usr/bin/env python3
import hashlib
import sys
import tarfile
from pathlib import Path

_CHUNK_SIZE = 1024 * 1024


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        while True:
            chunk = handle.read(_CHUNK_SIZE)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def sha256_archive_member(archive_path, member_name):
    digest = hashlib.sha256()
    with tarfile.open(archive_path, mode="r:*") as archive:
        try:
            member = archive.getmember(member_name)
        except KeyError as exc:
            raise ValueError(f"bundle member missing: {member_name}") from exc
        if not member.isfile():
            raise ValueError(f"bundle member is not a regular file: {member_name}")
        extracted = archive.extractfile(member)
        if extracted is None:
            raise ValueError(f"bundle member cannot be read: {member_name}")
        with extracted:
            while True:
                chunk = extracted.read(_CHUNK_SIZE)
                if not chunk:
                    break
                digest.update(chunk)
    return digest.hexdigest()


def verify_bundle_install(archive_path, binary_path, expected_archive_sha256,
                          member_name="codeql/codeql"):
    archive_path = Path(archive_path)
    binary_path = Path(binary_path)

    if not archive_path.is_file():
        raise ValueError(f"verified CodeQL bundle archive missing: {archive_path}")
    if not binary_path.is_file():
        raise ValueError(f"installed CodeQL executable missing: {binary_path}")

    actual_archive_sha256 = sha256_file(archive_path)
    if actual_archive_sha256 != expected_archive_sha256:
        raise ValueError(
            "CodeQL bundle archive checksum mismatch: "
            f"expected {expected_archive_sha256}, got {actual_archive_sha256}"
        )

    expected_binary_sha256 = sha256_archive_member(archive_path, member_name)
    actual_binary_sha256 = sha256_file(binary_path)
    if actual_binary_sha256 != expected_binary_sha256:
        raise ValueError(
            "installed CodeQL executable differs from checksum-verified bundle"
        )


def main(argv):
    if len(argv) not in (4, 5):
        print(
            "usage: verify_bundle_install.py "
            "<bundle.tar.gz> <installed-binary> <expected-bundle-sha256> "
            "[bundle-member]",
            file=sys.stderr,
        )
        return 2

    archive_path = argv[1]
    binary_path = argv[2]
    expected_archive_sha256 = argv[3]
    member_name = argv[4] if len(argv) == 5 else "codeql/codeql"

    try:
        verify_bundle_install(
            archive_path, binary_path, expected_archive_sha256, member_name
        )
    except (OSError, tarfile.TarError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    print("CodeQL extracted executable integrity: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
