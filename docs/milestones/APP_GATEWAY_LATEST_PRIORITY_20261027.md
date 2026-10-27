# ORUN Gateway → Android: current position first, history in background

Date: 27 October 2026. Stage: **code ready for independent host validation, NOT field-qualified**.
Base: unified Android map PR #184, commit `48f0a3b`.

## Owner's observation and decision
- **Esri Current + source zoom 18** gave the owner the best rural map imagery.
- The Gateway's initial DUMP sends history in **ascending** custody order; in the app, the newest point only becomes visible once it reaches the phone. During a continuous DUMP, an old React timer could also starve map refreshes.
- Desired flow: show the most recent available position early, keep the COMPLETE historical walk oldest to newest, and surface later genuinely new records while history continues. Never drop records or weaken verified custody ACK.

## Root causes confirmed in code
- `sf5l0ForEachHeld()` sorts custody pages in ascending generation and slots ascending.
- `Sf5l0EdgeLinkSession::nextHeld()` transmits oldest first, and `BENCH GW DUMP` prints the same order.
- `usePoints()` previously cancelled and re-created its 2-second map refresh timeout after every version increment. Under repeated packets this might delay visible updates until the stream quieted.
- Backend upload `Uploader` is already newest-first by durable phone ledger rowid; changing HTTP upload order **does not** fix Gateway→phone latency.

## Two transport lanes, without dropping history
- An **optional**, backward-compatible `HELD_PREVIEW (0x06)` shares the byte-for-byte original HELD payload structure (page, slot, generation, live link flags, exact encrypted object) but **never** adds to the history `DUMP_END` count.
- On BLE connect/DUMP, the Gateway sends the most recently durable **PERIODIC** record first (scheduling hint from the outer header only, not trusted; the phone authenticates the entire record before accepting). A live append schedules another preview.
- `PRIORITY` is the analogous standalone USB line printed before the unchanged `EDGE HELD` chronological listing and `EDGE DUMP END`.
- The existing archive stream remains in its original oldest-first order. A priority preview may recur as an ordinary archive HELD, is deduplicated by the same durable-ledger identity, and can be independently ACKed only **after SQLite COMMIT** and readback.
- BLE grants at most one priority preview ahead of one history item, with an explicit fairness latch; incoming positions cannot continually reset the history cursor.
- `DUMP_END` counts only ordinary HELD announcements, even when a preview was already accepted and the Gateway has reclaimed that record before its archived turn.
- Older phones ignore the new optional BLE type / USB PRIORITY line; they continue receiving the full chronological backlog.
- Partial/corrupt previews are dropped. Acceptance is unchanged: sealed object authenticator, replay/dedup, exact MAC handoff; the Gateway does **not** decrypt GPS measurements.

## Application display
- A leading+trailing **2-second throttle**, instead of a debouncer that resets on each new record, means the phone refreshes its map while the rest of the DUMP is still arriving.
- `buildHerd()` already selects the maximum **authenticated measurement timestamp**, not newest phone arrival. Older records never move a displayed animal backwards in time.
- The default for **new installs** is Esri Current z18, with previously persisted owner preferences preserved. No unrelated styling change.

## Important limitations
- The Gateway cannot see encrypted GNSS measurement time. Its priority record is **the last durably admitted PERIODIC custody record**, not a cryptographically guaranteed newest field measurement, and is not necessarily the latest point for every animal in a multi-collar herd. True per-device latest snapshot is a separate scheduler enhancement.
- No live LoRa bypass to an unavailable Gateway is asserted. Current firmware/Edge/Android bench security and compatibility constraints apply.
- **This is NOT an Android-only update**. The priority lane needs a corresponding Gateway firmware build and an authorized/tested flash. A phone-only update does not change a Gateway that still sends FIFO.
- Do not flash firmware remotely/automatically. Verify board role, grants and existing flash storage before any field update. No erase or re-provision commands are required by this code change.
- App remains without phone-generated duplicate geofence OUTSIDE alerts (owner decision from PR #184).
- Regional offline satellite map download is not added here.

## Required checks before APK/firmware deployment
On a clean checkout of **this exact branch/commit**:

```bash
cd edge/app
npm ci
npm test
npm run build
cd ../../
bash tools/ci/run_sf5l6_ble_edge_host.sh
bash tools/ci/run_sf5l7_edge_app_host.sh
cd firmware
pio run -e rak4630
```

Device/Gateway qualification:
1. Existing 40+ held PERIODIC records: `HELLO → 0x06 preview of last eligible periodic → ordinary 0x02 HELD oldest..newest → DUMP_END`. USB: `PRIORITY → HELD oldest..newest → DUMP END`.
2. Preview persists in SQLite and map **before** old archive finishes; no misleading `LIVE` label for a stale fix (measurement-time freshness remains authoritative).
3. A brand new custody append during historical BLE transfer schedules prompt preview and does not indefinitely starve archival progress.
4. ACK/reclaim, disconnections, retries, duplicate previews, malformed frames and dump counters all behave as prior regression contract.
5. Old Android APK (or PC client) still accepts full original history, ignoring optional preview.
6. Use Esri Current/z18 in the field; save preference persists after restart.
7. Verify actual RAK4630 gateway board is test-approved before any flash; preserve flash provisioning and held records.

**Validation gate: tests listed above have NOT yet been run against this branch on the owner's machine. No new APK or firmware binaries claimed.**
