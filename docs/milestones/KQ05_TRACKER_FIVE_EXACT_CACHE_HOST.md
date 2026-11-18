# KQ05 Tracker OBS1 exact protected-object cache: 4 -> 5

**Status: DRAFT, PC host verification pending. No firmware upload.**
Parent PR #346 at `2f2a64b7eb76e7f2796a0da4c85ff669f0b54450`.
The owner verified Gateway `ProductCustodyStore` 1..5 true durable
Flash readback, scoped grant recheck, batch HMAC **PASS** on PC.

## Why this narrow change

KQ05 Gateway can now construct a proposed five-record batch ACK after
actual durable readback in an isolated PC proof. Tracker's existing
OBS1 control journal persisted **at most four** full 84/100-byte exact
PRODUCT_SECURE frames. The four-slot bound is a SOFTWARE POLICY, not
a physical 4-slot page: each of two control pages has 28 physical
144-byte record slots. The reserved control limit was:

- Exact object cache: 4
- Open occurrences: 8
- Result guards: 8
- StoreState: 1
- Total active controls: 21, spare per page: 7

This change raises ONLY `osf::kMaxExactCustodyObjects` to 5:
- Total active controls: 22, spare per page: 6
- OBS1 data/control wire format, control compaction rules, physical
  primary 128 KiB / supplementary 28 KiB addressing unchanged
- No automatically released observations, no loss of an old cached
  object, and **no silent eviction of a sixth object**

The existing `snapshotRetainedExact()` and
`ProductRecordStore::snapshotCustodyCandidates()` use the shared
constant, so their exact frame buffers are now sized for 5.
Existing single-object custody ACK logic is still independent.

## Host proof

`bash tools/ci/run_kq05_tracker_five_exact_cache_host.sh`

Builds the **actual** `ObservationStore`,
`observation_store_control` and `observation_store_format`
under target-compatible GNU++11 and executes the original comprehensive
SF5C control-journal test extended with:
1. Append six logical observations.
2. Commit exactly five exact protected frames in OBS1 control Flash.
3. Confirm a simultaneous **five-item** read-only snapshot.
4. Cold mount the same fake NOR region and check all five exact byte
   sequences recovered identically.
5. Reject the sixth object without erasing or evicting any of the
   five held records.
6. After an independently durable release marker on record #1,
   permit the sixth object to take its freed cache slot, never sooner.

## What this does NOT solve

A capacity of five is still bounded, not an unbounded custody queue.
A lost batch ACK can pin five exact bytes until a proper authenticated
retransmitted ACK is received. The device's remaining, older plaintext
observations can still exist in 156 KiB logically, but they do not
automatically have five additional durable ciphertext control slots.

The next security-gated stage must:
- Authenticate type `0x0A` against Tracker's real durable
  SecurityStore V3 custody scope and the correct derived
  **per-Tracker/per-Gateway scoped key**;
- Resolve all five exact cached frames uniquely, match all
  FULL SHA256 digest values, and fail closed if any is absent;
- Revalidate the current scope before each durable release marker,
  which the SINGLE ProductRecordStore loop owner must commit and
  reread. No whole-range or floor-based deletion.
- Process duplicate/lost/partial receipt and multiple Gateway cases,
  with new live telemetry always prioritized over backlog.
- Qualify actual radio airtime and on-device Flash power failure.

This patch DOES NOT implement radio reception, release or actual 28KiB
runtime cutover and DOES NOT claim field readiness. P0 remains BLOCKED.
