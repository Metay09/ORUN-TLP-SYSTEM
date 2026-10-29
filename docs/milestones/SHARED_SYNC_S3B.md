# ORUN S3B — Authenticated shared-sync HTTP client (isolated candidate)

**Status:** DRAFT / HOST TESTS NOT YET RUN.
**Base:** S3A PR #196 + Step 3C preflight PR #195 + S2 backend PR #194.
**Data:** user-drawn zone/group/target DRAFTS only. No Tracker mutation, no
geofence APPLIED status and no new OUTSIDE alarm source.

## Code

- `edge/app/src/app/sharedSyncClient.ts`: `SharedSyncClient` transports the
  offline `OfflineSyncStore` queue and backend cursor feed.
- `edge/app/test/sharedSyncClient.test.ts`: injected HTTP responses exercising
  A-to-B E7 sync, no auth, wrong site/install, 401/403, timeouts, redirects,
  duplicate overlapping calls, invalid result, local storage failure,
  pagination, tombstones and original local draft preservation.

## Security boundaries

- A separately verified Step 3C enrolled session provider supplies per-device
  token **at time of the explicit operation**. The client never extracts
  identity/authorization from an arbitrary HTTP header, public IP or ORUN
  Edge bench token. It verifies site and installation equal its local mirror.
- Only HTTPS origins accepted remotely. Exact localhost/127.0.0.1/::1 HTTP
  is allowed for isolated host testing; there is no plain-HTTP Tailscale
  exception in the client.
- Credentials are omitted from URLs, request logs, UI errors and shared
  data. `fetch` uses `redirect:"manual"`, `credentials:"omit"`,
  `cache:"no-store"`, a bounded 15-second AbortSignal timeout, explicit
  application/json and response-size bound.
- `POST /api/v1/sync/ops` is bounded to 50 ops and 120 KiB per pass;
  server's accepted `op_id`, revision and sequence are checked before
  clearing the local outbox. Lost replies are retried using the SAME op_id.
- `GET /api/v1/sync?cursor=N&limit=100`: monotonic, bounded pull,
  persisted cursor + resources as ONE local record. No cursor advance on
  invalid data or failed local write. Up to 10 pages per explicit call;
  `moreAvailable` prevents incorrectly claiming a full catch-up.
- Server read permission is separate from write permission. Any 401, 403,
  429/5xx or network exception preserves queued originals.
- Every call to `runOnce()` is **opt-in**, not a startup effect or
  automatically repeating timer. A second overlapping call is rejected.
- `SharedSyncClient` never writes the existing `orun.app.zones.v1` or
  `orun.app.zoneTargets.v1` storage keys. Remote mirror updates are not
  shown as a replacement for the user's original drafts until a later
  reconciled UI migration.

## Missing before actual multi-phone ORUN

1. Real Android Keystore Ed25519 enrollment, owner comparison/approval UI,
   token protected storage, refresh/rotation and TLS deployment.
2. Production-grade SQLite outbox transaction, restart and power-loss audit.
3. Client app lifecycle integration, authenticated HTTP endpoint in deployed
   backend, browser CORS/origin policy and sync status/UX.
4. Existing field modifications captured transactionally, canonical incoming
   resource validation and non-destructive UI cutover; no silent overwrites.
5. Real Android A/Backend/B test and authorization revocation/access controls.

Nothing here starts a server, edits an existing on-device area, changes
running `orun-backend.service` / `orun-pc-edge.service`, or sends USB/BLE/LoRa
commands.

## Owner test command

```bash
cd edge/app
npm test
npm run typecheck
npm run build
```

A **host PASS** of these tests proves only contract behavior with an injected
fetch transport. It is not evidence of real internet synchronization.
