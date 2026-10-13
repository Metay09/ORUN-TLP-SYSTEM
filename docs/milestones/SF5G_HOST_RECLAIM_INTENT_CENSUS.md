# SF5G — Reclaim intent journal census, host-only

Date: 2026-10-13
State: **CANDIDATE; HOST TESTS NOT YET RUN ON THIS HEAD**.
Base: `main@120cb1d28626a94c92f369ebc3c0d66a0494ca26`
(append-only multi-page host model from merged PR #122).

## Purpose and narrow result

The current SF5G page format reserves **three 24-byte intent slots**
in the last 72 bytes of each 4 KiB page. Real reclamation is not yet
safe: structural intent markers are NOT proof of authenticated Edge
delivery or durable absence of HELD packets. This slice implements a
**read-only diagnostic census** of this reserved area, without ANY
flash erase, intent write/completion, custody release, or ACK API.

Files:
- `firmware/tests/sf5/sf5g_intent_census_candidate.h/.cpp`
- `firmware/tests/sf5/test_sf5g_intent_census_candidate.cpp`
- `firmware/tests/run_host_tests.sh` adds one isolated executable.

The reader accepts only:
1. A contiguous prefix of structurally **COMPLETED** intents.
2. Optionally one **COMMITTED-but-not-completed** intent.
3. A contiguous suffix of completely **ERASED** slots.

It independently checks target index against caller-provided actual
physical page count (1..32), rejects self-targeting and unsupported
codec fields, and fails closed on staged/torn/corrupt/noncontiguous
entries or read failure. It reports:
- `kAvailable`: at least one erased intent slot, no pending intent.
- `kPending`: a single committed but not completed intent.
- `kExhausted`: all three slots are structurally completed.
- `kRefused` / `kReadFailure`: no safe classification.

**Even `kAvailable` or `kExhausted` provides NO erase permission.**
`kCompleted` is only a flash marker and is NOT evidence that target
erase completed, an Edge ACK was authenticated, or source generation
and ownership match. This checker does **not** read or validate the
source OCP3 header or the target page and cannot issue a reclaim plan.

## Host tests

- Empty journal, one completed, one pending, resume after pending
  completion, and three completed slots (= exhausted, no reuse).
- Crash simulation at every attempted byte of a first intent's staged
  24B payload, 4B commit marker and 4B done marker (0..32).
- Refuse staged/partial commits, partial completion, noncontiguous
  intents, concurrent committed requests, corrupted CRC,
  self-targeting, target outside actual physical region,
  invalid source index/count, and simulated read failure.

No `firmware/src`, `firmware/include`, `platformio.ini` or
physical device modifications. The test only injects changes into
host NorPage objects; the **census itself is read-only**.

## Integration and safety gates still open

The existing `PageWriter::reservedAreasErased()` intentionally
rejects pages carrying any intent bytes. As such, this is an
**isolated parser/test improvement**, NOT an integrated RegionWriter
reclaim path. Never infer that N1 liveness/reuse is solved.

Before implementing actual erase/reclaim:
- Full owner/magic/version/generation validation of source+target.
- Complete authenticated Edge custody-receipt and physical
  durable/readback chain; **zero HELD objects** may be lost.
- A state machine that handles full intent journal (N1), torn
  handoff markers (N2), async begin (N3), read failure (N4), and
  bounded time/latency (N5).
- Crash-safe reclaim intent staging, commit, actual target erase,
  readback proof, and complete marker; no erase solely from markers.
- A model for interrupted NOR erase and physical hardware validation.
- Multi-page FIFO/rollover that survives generation wrap/relocation;
  no implicit reuse of torn or foreign pages.

We will perform the agreed independent security audit at a later
**storage-critical integration milestone**, not for each isolated
host-only regression.
