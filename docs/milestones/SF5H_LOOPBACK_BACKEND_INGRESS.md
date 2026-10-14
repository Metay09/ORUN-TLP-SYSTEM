# SF5H — Loopback Edge → Backend Ingress Staging (host-only)

Date: 2026-10-14
Status: **DRAFT candidate / exact-HEAD full host validation pending**.
Base: `main@6c46d28508d5474c6d3cea50ba31af917d4399c7` (PR #127).

## Why this work exists

The Edge SQLite candidate (PR #126) durably persists exact 84/100B
`PRODUCT_SECURE` ciphertext on an ordinary Linux host (subject to
SQLite/filesystem assumptions). PR #127 adds read-only, full-queue
validated FIFO retrieval. Next, show that a **real byte transfer
across a local HTTP socket into a different SQLite file** works while
preserving all original Edge records through timeouts, errors and
duplicate retransmissions.

This is **NOT the canonical ORUN Backend**. The authoritative SF4A
architecture requires security-verified device/epoch identity,
decryption and semantic logical-record idempotency at the Backend,
plus authenticated Backend durable acceptance before Edge release.
This candidate deliberately implements **only local untrusted
opaque *staging*** and never claims authenticated acceptance.

### Files

- `edge/candidates/loopback_backend_ingress.py`: test-only
  `LoopbackBackendServer`, `LoopbackEdgeSender`, and typed
  untrusted staging results.
- `edge/candidates/edge_durable_inbox.py`:
  `oldest_batch_after(volatile_after_sequence, limit)` allows
  bounded FIFO iteration across multiple read windows, while
  rechecking ALL retained rows on each request. `oldest_batch()`
  remains a wrapper over sequence zero. This is **not** a durable
  delivery checkpoint or purge authorization.
- `firmware/tests/sf5/test_sf5h_loopback_backend_ingress.py`:
  reproducible real localhost HTTP+SQLite integration tests.
- `firmware/tests/run_host_tests.sh`: includes the test.

## Contract and boundaries

1. Local server binds **only** IPv4 `127.0.0.1`, OS-selected test
   port; outgoing client refuses `0.0.0.0`, DNS, LAN/WAN addresses,
   and non-integer/invalid ports. The HTTP protocol is cleartext with
   **NO authentication**: localhost containment is *not* a secure
   trust boundary. Do NOT use this listener in production, expose it
   through reverse-proxy/tunnel, Docker published ports or forward it
   to mobile devices.
2. Local POST path `/v0/stage` accepts a bounded exact-length raw
   84B or 100B custody-requested product packet, and puts it in a
   separate backend **opaque staging spool** using the same SQLite
   code as the Edge's candidate inbox.
3. Transaction/commit/reopen/readback precedes returning an HTTP
   201 inserted / 200 duplicate staging diagnostic. Client checks
   status, `kind`, echoed length and SHA-256 for consistency,
   **not cryptographic authentication**. SHA-256 does not establish
   protected origin, session/channel identity or Edge/backend
   custody authority.
4. Receiver uncertainty returns HTTP 503; full returns 507;
   invalid input returns 400/422. An injected fault before commit
   leaves no record. A fault after commit may leave a record but
   returns 503 and causes a safe retry/duplicate.
5. Sender reads Edge FIFO without changing it; on network error,
   malformed response, backend full or any storage uncertainty it
   stops at the first failed item. The returned volatile sequence is
   only a local iteration cursor. Starting again at zero safely
   retransmits identical ciphertext: the backend *staging* spool
   deduplicates by **full exact bytes and length**, not hash only.
6. Both databases retain all committed records regardless of HTTP
   response; **NO delete, reclaim, custody ACK, authenticated Edge
   accept, backend durable-accept receipt or user-visible delivery
   claim exists** in this implementation.
7. The backend *staging* spool cannot merge different encrypted
   objects that decrypt to the same logical observation. Only a
   future authorized decrypting canonical Backend owner may apply
   product-family, account/credential, epoch, re-protection and
   content-aware logical idempotency rules. `RESULT` remains out
   of scope.

## Host test cases

- Seventy exact 84B/100B objects over **two** FIFO send windows
  (64+6), real loopback POST to second SQLite file; verify byte
  equality and chronological order, and retention at both ends.
- Sender/Edge database reopened, lost in-memory cursor, resend all
  exact ciphertext: receiver reports duplicate, no extra rows.
- Before-commit failure produces no backend row, retried later.
- After-commit response loss returns error even though one row was
  written; retry recognizes exact duplicate.
- Receiver capacity exhaustion fails at the first unsent item and
  preserves all source objects.
- No receiver listening: report failure; source unchanged.
- Tail-corrupt Edge queue refuses sending even first item.
- Malformed/fake local success with wrong fingerprint is refused.
- Reject non-loopback client host/ports and bad HTTP input.

## Production engineering gates (explicitly OPEN)

- Real authenticated transport and enrolled Edge/backend principals
  with replay-safe, nonce/epoch-scoped receipt contract.
- Canonical backend decrypt/authenticate and semantic dedup of
  logical observation identity; content mismatch is a diagnosed
  integrity conflict, not `first-writer-wins`.
- Real receipt binding and transactional Edge durable release only
  after authenticated backend durable acceptance, including safe
  restart state and corruption handling.
- Background sync scheduling, retry/backoff, bounded memory and
  I/O performance, remote network, IPv6/proxies, certificates,
  connection loss/timeout budgets, throttling and quotas.
- Independent security/storage review, physical power-cut and
  filesystem/SQLite WAL/full synchronous validation before any
  production use. Do not interpret a host commit/reopen test as
  actual removable-media or interrupted-fsync qualification.
- Phone app/USB/BLE transport, actual roaming Gateway bridge and
  RAK4630 hardware integration.

This PR introduces **no device firmware code, hardware erase,
Gateway owner change, application delivery or real user credentials**.

## Merge gate

Run the entire `bash firmware/tests/run_host_tests.sh` on the exact
branch HEAD. Require `HOST_EXIT=0`, eight SF5G PASS, three SF5H
PASS (inbox, FIFO, loopback ingress) and no traceback/assertion or
sanitizer failure. The new line should read:

`SF5H loopback backend ingress: live-HTTP/70-FIFO/retry/full/fault/no-release: PASS`
