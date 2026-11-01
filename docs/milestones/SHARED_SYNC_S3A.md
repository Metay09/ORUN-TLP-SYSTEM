# ORUN S3A — Android offline shared-sync core and safe legacy staging

**Status:** HOST VALIDATED 2026-10-29 (owner: 82/82 Android tests, typecheck and Vite build PASS at `2719a25`); real Android sync/login NOT YET INTEGRATED.
Built above PR #195 Step 3C host preflight and PR #194 SharedStore backend.
Owner decision: 30-day fence what-if simulation remains OUT OF SCOPE.

## New files

- `edge/app/src/app/offlineSync.ts`: pure TS zone/group/zone-target mapping,
  an installation-and-site-bound local sync mirror, operation outbox,
  acceptance/conflict handling, monotone server change cursor and
  **explicit single-write legacy staging**.
- `edge/app/test/offlineSync.test.ts`: real E7, migration, storage failures,
  accepted/rejected responses, site binding and tombstones.

## Non-destructive migration contract

The original real Android keys are:
- `orun.app.zones.v1`
- `orun.app.zoneTargets.v1`

This candidate **never modifies or deletes either key**.
`stageLegacy(zones,targets,freshId)` is opt-in, called only after future
owner-approved Step 3C enrollment; all entries are validated before ONE
atomic `storage.setItem` on a NEW sync key. If any record is invalid,
duplicate, missing group, storage unavailable or corrupt, it fails closed.
The current `ZoneStore` and `ZoneTargetStore` remain active.

A newly enrolled phone with an old local area set may stage `base_revision=0`;
if the same stable resource ID already exists in the backend, it gets
`CONFLICT` and the **local original remains untouched**. No silent
last-writer-wins. There is no automatic merge, no implicit CLEAR, no claimed
Tracker `APPLIED`.

The `OfflineSyncStore` prototype holds mirror, outbox, problems and cursor
under one browser storage key. A write failure is treated as a failed local
transaction and leaves the old in-memory state, old persisted document and
original zones unchanged. A repeated HTTP request can be retried with the
same `op_id` once the authenticated HTTP adapter exists.

## Owner host verification evidence (2026-10-29)

- Commit tested: `2719a25`.
- `npm ci --no-audit --no-fund`: PASS (176 packages; only a dependency deprecation warning).
- `npm test`: **82/82 PASS**, including **9/9 new offlineSync tests**.
- `npm run typecheck`: **PASS**.
- `npm run build`: **PASS** (non-blocking bundle chunk-size warning).
- No live network connection, Step 3C phone enrollment, Android runtime write or production DB change was tested or made.

## Deliberate boundaries

- **NOT wired into `makeSources`, screens, Android lifecycle or network**:
  the user's working app remains local until the new login/permissions and
  migration integrity are tested.
- Browser localStorage can be a prototype store for tiny geofences, but
  **production durable multi-step migration/outbox requires an assessed
  SQLite / Capacitor bridge, crash/fault-injection tests and atomic movement**
  from old keys to the validated shared mirror. This is not yet that bridge.
- No Android Keystore Ed25519 identity UI, token transport, authenticated
  `fetch` integration or TLS deployment. Do not reuse the bench token.
- No automatic two-way sync polling, no legacy-to-mirror UI cutover and no
  Web/PC screen. No server/service stop or live SQLite mutation.
- Existing drafts may be edited concurrently in the old UI; opt-in staging
  captures only a snapshot. Before migration cutover, additions must be
  captured safely without silently losing late edits.
- Backend S2 currently implements only `zone`, `group`,
  `zone_target` shared draft subsets; broader application objects remain
  separate milestones.
- Server accepted resource != acknowledged by Tracker. No USB/BLE/LoRa
  command side effects and no geofence alarm authority in this module.

## Validation

At repository root of the isolated verification worktree:
```bash
cd edge/app
npm test
npm run typecheck
npm run build
```
Test additions are in `test/offlineSync.test.ts`.

Next milestones: choose and audit durable Android storage, Step 3C identity UI,
protected token storage, authenticated shared-sync HTTP adapter, concurrent edit
and crash migration. Then validate A draws offline → backend accepts → B/web
receives identical E7 geometry/revision and denied phones never see it.
