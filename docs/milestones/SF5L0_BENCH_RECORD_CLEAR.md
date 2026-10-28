# SF5L0 development bench product-record reset

**Scope:** development Tracker only, `rak4630_sf5l0_tracker_bench`.
Not production firmware. Base is stacked PR #190 (which depends on #189).
No SecurityStore, ConfigStore, GeofenceStore, legacy HistoryStore,
Gateway grant, D2A counter or backend database reset is authorized here.

## Rationale

The original `PRODUCT STORE PREPARE CONFIRM` deliberately refuses a healthy
mounted ObservationStore. The physical Tracker is working but carries hundreds
of bench-created records and an exact-object cache. Reprovisioning or whole-chip
erase would destroy unrelated security state, so the test-only clear must be
scoped to `storage_config::kProductRegionPages`.

## Guarded USB command

`BENCH TRACKER ERASE RECORDS CONFIRM` is compiled only with
`ORUN_SF5L0_TRACKER`. It requires:
- `APP SERVICES NONE` applied (tracking stopped and persisted);
- `BENCH DRAIN OFF` (send pacing stopped);
- no pending custody ACK or pacer waiting for one;
- healthy mounted observation-owner region (never foreign format);
- no pending periodic/cache/release write, unconsumed outcome, or uncertain flash.

The implementation reuses `ProductRecordStore::drivePrepare()` and erases
ONLY the 32 product pages. It prints a prepare result and reboots after successful
verified erasure. On reboot a freshly generated ObservationStore incarnation and
new sequence series begin. Existing security credentials and counters remain.

## Required verification before device use

```bash
bash tools/ci/run_sf5l0_bench_clear_host.sh
cd firmware
pio run -e rak4630_sf5l0_tracker_bench
```

No physical firmware image should be uploaded solely on the strength of source
review or this host test. Confirm toolchain, output, and device ID. The user
must perform the physical flash and clear because the linked GitHub connector
cannot operate the Linux USB device.

## Physical operator sequence (after build and review)

1. Verify Tracker ID `0E8ADE7E71531AA3`, Gateway ID
   `09A462BD4B275BA5`; do not swap ports.
2. Ask `BENCH DRAIN?` and `PRODUCT STORE?` for baseline.
3. Send `APP SERVICES NONE`; verify `APP SERVICES?` reports tracking OFF.
4. Send `BENCH DRAIN OFF`; verify `BENCH DRAIN?` shows OFF,
   `waiting_ack=no`.
5. Send `BENCH TRACKER ERASE RECORDS CONFIRM` once. Observe the
   `PRODUCT STORE PREPARE result=PASS; rebooting to mount` line.
6. After reset verify `PRODUCT STORE?` shows `retained=0`, `lost=0`,
   `next_seq=1`, fresh `inc`, and `BENCH?` still says
   `security=PROVISIONED` and the same `granted_gateway`.
7. Restore tracking with `APP SERVICES T` after verification. Bench pacing
   may default ON after reboot: check `BENCH DRAIN?`.

Gateway holds and backend-stored history are not erased by this command.
Separate cleanup of verified test records in those systems needs its own
scoped command; never remove operational credentials or blind-drop
unconfirmed custody.

**Status:** proposed/draft, hardware tests required.
