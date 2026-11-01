# ORUN S3C — True Python HTTP / Node TypeScript shared-sync E2E gate

**Status:** OWNER LOOPBACK REAL-HTTP HOST PASS (2026-10-29, tested `f43244d`). Physical Android / Tailscale production sync not yet integrated.
**Depends on:** PR #194 Backend S2 (host 19/19), PR #195 Step3C security
(host 17/17), PR #196 Android S3A (host 82/82), PR #197 Android S3B
(host 93/93 + typecheck + Vite build PASS).

## Why this gate exists

S3B tested the client against injected fake `fetch` responses. This new
test uses **actual socket HTTP**, the actual `backend/shared_sync_http.py`
adapter, real `SharedStore` transactional SQLite, a **real Ed25519**
Step3C owner-approved registry, and the actual TypeScript
`SharedSyncClient` via Node native `fetch` with no stub.

## Isolated components

- `tools/ci/test_shared_sync_real_http_e2e.py`: creates a private temporary
  directory with two SQLite databases; enrolls four unique installations by
  the existing `EdgeEnrollment` challenge/owner approval/Ed25519 flow;
  starts Python `ThreadingHTTPServer(("127.0.0.1", 0))` on an ephemeral local
  port only; invokes Node and then verifies owner revocation over real HTTP.
- `tools/ci/shared_sync_e2e_client.ts`: uses actual `OfflineSyncStore`
  and `SharedSyncClient` with native Node fetch against that Python port.
  Two phone mirrors, a distinct-site phone, and a same-site PC editor are
  separate in-memory instances.
- `tools/ci/run_shared_sync_real_http_e2e.sh`: explicit test runner.

## Acceptance

1. A enrolls under site 1, stages local area/group/target without touching
   original draft keys, sends 3 backend mutations using a real 3C token.
2. B, separate enrolled and **read-only**, pulls exactly the real E7 area,
   group, and target rule, across independent SQLite HTTP reads.
3. Reopening B's local mirror retains accepted state; subsequent delta
   pull does not duplicate changes.
4. C under another site cannot read A/B records, even when it creates the
   same zone ID in its own site.
5. A updates zone to rev2; B pulls rev2. PC editor D, holding a stale rev1,
   gets CONFLICT and retains its unsent proposed geometry for manual review.
6. Read-only B tries writing: server rejects, pending edit preserved for
   review and does not turn into a Tracker `APPLIED` state.
7. Server revokes A's enrollment: A's old bearer is denied (401), B's still
   works (200). An unenrolled bench token gets 401.
8. Server is **not bound to Tailscale, public IP, Android or live Backend**.
   Temporary DB and any test tokens are discarded after the test. No
   `orun-backend.service`, `orun-pc-edge.service`, Tracker, Gateway, BLE,
   LoRa, real phone storage or persistent DB is changed.

## Owner command

From an isolated checkout at the PR head:

```bash
bash tools/ci/run_shared_sync_real_http_e2e.sh
```

Requires Python 3 + `cryptography` and Node 22+ with TS stripping, both
available on the owner's previously verified workstation. No new pip/npm
dependencies needed.

## Owner real-HTTP result (2026-10-29)

- User Debian checkout `f43244d` invoked `bash tools/ci/run_shared_sync_real_http_e2e.sh`.
- `SHARED_SYNC_REAL_HTTP_E2E: PASS`: owner staged/uploaded 3/3 zone/group/target resources; authorized second phone mirror pulled 3/3; other site isolated; stale PC edit preserved as conflict; read-only user write denied; client mirror persisted after reopen.
- `STEP3C_REAL_HTTP_E2E: PASS`: revoked phone token returned 401; second enrolled phone remained authorized.
- Test bound IPv4 loopback and used temporary SQLite. No running backend, PC Edge or production device modified.
- The result proves Node/native HTTP interoperability, **not live Android WebView, TLS deployment or user enrollment UX**.

## Still not production

This is a **loopback host qualification**, not real Android-to-Backend sync:
there is no Android Keystore enrollment UI, production HTTPS/Tailscale route,
Android SQLite atomic outbox and app integration, server origin/CORS review,
multi-account Device Registry ACL, or actual two physical phone installation
test. Those remain separate work items. This test must never be used as a
reason to enable unencrypted Tailscale HTTP or auto-import old app data.
