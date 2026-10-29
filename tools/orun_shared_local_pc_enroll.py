#!/usr/bin/env python3
"""ORUN S4D — explicitly owner-authorized, local-PC-only reader enrollment.

Trusted LOCAL OPERATOR command, never exposed as an HTTP endpoint. Requires
a separate, pre-existing 0700 shared-backend state directory with the pair of
0600 SQLite databases (S4C). This tool generates an ephemeral Ed25519 key,
exercises the real Step3C begin/owner-approve/challenge/finish flow and writes
the resulting 30-day bearer once to an exclusively-created 0600 token file.

Private Ed25519 key and owner code live only in this invocation's memory;
neither are exported. No phone/Gateway enrollment is performed: the
installation ID MUST have the 'pc-' prefix and scopes are READ-ONLY.
A failed token write attempts to revoke the issued credential.

Note: This local-only shortcut is NOT remote phone enrollment, multi-user
owner approval UI, secure token refresh, TLS provisioning or a production
deployment. It is a constrained path for an owner operating their OWN PC.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import stat
import sys

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from backend.edge_enrollment import EdgeEnrollment, EnrollmentError, signed_challenge  # noqa: E402
from backend.shared_sync_store import Actor, SyncForbidden  # noqa: E402
from tools.orun_shared_sync_loopback import (  # noqa: E402
    FILES, StartupRefused, prepare,
)
from tools.orun_shared_pc_mirror import ID  # noqa: E402

READ_ONLY = frozenset({"zone:read", "group:read", "zone_target:read"})


class LocalPcEnrollError(RuntimeError):
    """Safe constant-code operator error. Never contain credentials."""


def _private_destination(path: Path) -> None:
    if not path.is_absolute() or path.is_symlink() or path.exists():
        raise LocalPcEnrollError("TOKEN_OUTPUT_MUST_BE_NEW_ABSOLUTE_FILE")
    parent = path.parent
    if parent.is_symlink() or not parent.is_dir():
        raise LocalPcEnrollError("TOKEN_PARENT_INVALID")
    st = parent.stat()
    if st.st_uid != os.geteuid() or stat.S_IMODE(st.st_mode) & 0o077:
        raise LocalPcEnrollError("TOKEN_PARENT_MUST_BE_OWNER_ONLY")
    if path.name in ("", ".", ".."):
        raise LocalPcEnrollError("TOKEN_OUTPUT_INVALID")


def authorize_local_pc(state_dir: Path, *, site: str, actor: str,
                       installation: str, token_out: Path,
                       explicitly_approved: bool) -> str:
    """Return nonsecret public-key fingerprint after writing a 0600 bearer.

    Explicit approval is a trusted local owner action. No public network API
    can invoke this function. It deliberately never returns the bearer.
    """
    if not explicitly_approved:
        raise LocalPcEnrollError("LOCAL_OWNER_APPROVAL_REQUIRED")
    if not isinstance(installation, str) or not installation.startswith("pc-") or (
            not ID.fullmatch(installation)):
        raise LocalPcEnrollError("LOCAL_PC_INSTALLATION_ID_REQUIRED")
    if not isinstance(site, str) or not ID.fullmatch(site) or (
            not isinstance(actor, str) or not ID.fullmatch(actor)):
        raise LocalPcEnrollError("INVALID_SITE_OR_ACTOR")
    _private_destination(token_out)
    # Reuse MUST be deliberate; this command never creates Backend state or
    # adopts the live ORUN observation database.
    _, registry = prepare(state_dir, reuse_state=True)

    # Reserve new destination BEFORE issuing a server bearer. O_EXCL prevents
    # accidental overwrite and rejects another process racing to create it.
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(token_out, flags, 0o600)
    except OSError as exc:
        raise LocalPcEnrollError("TOKEN_CREATE_FAILED") from exc
    enrolled = False
    try:
        private = Ed25519PrivateKey.generate()
        public = private.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
        pending = registry.begin(installation, public)
        # Only an explicit trusted shell invocation is an owner approval;
        # the authenticated public sync handler NEVER invokes approve().
        code = registry.approve(pending.request_id, pending.fingerprint,
                                site, actor, READ_ONLY)
        signature = private.sign(signed_challenge(pending, installation, public))
        token = registry.finish(pending.request_id, installation, public,
                                signature, code)
        enrolled = True
        # Defensive verification before persisting token: never record a
        # different site/installation or inadvertently grant write access.
        resolved = registry.resolve_token(token)
        expected = Actor(site, actor, installation, READ_ONLY)
        if resolved != expected:
            raise LocalPcEnrollError("ISSUED_IDENTITY_MISMATCH")
        with os.fdopen(fd, "wb") as f:
            fd = -1
            f.write((token + "\n").encode("ascii"))
            f.flush()
            os.fsync(f.fileno())
        return pending.fingerprint
    except BaseException:
        if fd >= 0:
            os.close(fd)
        if enrolled:
            try:
                registry.revoke(installation)
            except BaseException:
                # Never claim successful enrollment or leak the token;
                # manual recovery must inspect/revoke this installation.
                raise LocalPcEnrollError("CREDENTIAL_REVOKE_REVIEW_REQUIRED") from None
        token_out.unlink(missing_ok=True)
        raise


def main() -> int:
    p = argparse.ArgumentParser(
        description="Trusted-local owner approval of a READ-ONLY PC Edge installation")
    p.add_argument("--state-dir", required=True,
                   help="Existing separate 0700 S4C Backend state directory")
    p.add_argument("--site", required=True)
    p.add_argument("--actor", required=True)
    p.add_argument("--installation", required=True,
                   help="New pc-* installation, separate from phone/Gateway")
    p.add_argument("--token-out", required=True,
                   help="New absolute bearer path under pre-existing private 0700 dir")
    p.add_argument("--authorize-local-pc", action="store_true",
                   help="EXPLICIT OWNER APPROVAL: no phone or remote enrollment")
    opts = p.parse_args()
    try:
        fp = authorize_local_pc(
            Path(opts.state_dir), site=opts.site, actor=opts.actor,
            installation=opts.installation, token_out=Path(opts.token_out),
            explicitly_approved=opts.authorize_local_pc)
    except (LocalPcEnrollError, StartupRefused, EnrollmentError, OSError) as exc:
        reason = str(exc) if isinstance(exc, (LocalPcEnrollError, StartupRefused)) \
            else "LOCAL_ENROLLMENT_FAILED"
        print("ORUN_LOCAL_PC_ENROLL_REFUSED=" + reason, file=sys.stderr)
        return 2
    print("ORUN_LOCAL_PC_ENROLLED_READ_ONLY=YES")
    print("ORUN_LOCAL_PC_PUBLIC_FINGERPRINT=" + fp)
    print("TOKEN_SAVED_0600=YES;TOKEN_NOT_PRINTED=YES")
    print("NO_SYSTEMD_CHANGES;NO_ANDROID_OR_TRACKER_AUTHORITY")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
