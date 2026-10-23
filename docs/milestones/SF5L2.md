# SF5L2 — bench Tracker backlog drain (paced, one record at a time)

**Status:** code + host tests + **physical bench PASS (2026-10-24)**. Bench only (SF5L0 images).
**Base:** SF5L1 (`feat/sf5l1-gateway-usb-export`, PR #157).
**Why:** SF5L1 bench showed the one-record-per-boot pilot cannot keep up: the
collar's full ObservationStore overwrote 83 old records between two runs.

## What it adds

- `TrackerProductTxPilot::rearm()`: after a TX, select the oldest retained
  record once more. Released → next record. Not released → the SAME cached
  exact bytes again (no re-seal, no counter, no flash write). Blocked stays
  blocked. Not used by the default image.
- `Sf5l0DrainPacer` (bench): re-arm 3 s after a durable release; on no release
  within 15 s, retry the same record after 60 s; token bucket on product
  airtime (6 % long run, 60 s depth, 2.3 s per SF11 100-byte frame) so this
  sender stays under 10 % of any hour. v1 POSITION / HISTORY_SECURE airtime is
  not counted.
- Tracker commands: `BENCH DRAIN?`, `BENCH DRAIN ON|OFF` (default ON).
- Gateway: `BENCH?` shows `held=<n>/1056`; `BENCH GW ERASE HELD CONFIRM` wipes
  a READY custody store once the PC ledger has the records (bench stand-in
  for the missing authenticated Edge accept). Status split into two lines
  (256-byte printf limit).

## Limits worth knowing

- At 6 % the drain sends about 94 records/hour. A 1-minute report interval
  produces 60/hour, so a full backlog shrinks by only ~34/hour; a 15-minute
  interval (4/hour) lets it drain.
- The Gateway holds at most 1056 records and never releases them without an
  Edge accept; the collar has more than that. Empty it with the PC tool, then
  `BENCH GW ERASE HELD CONFIRM`.

## Evidence

- `test_sf5k3v_tracker_pilot.cpp`: rearm re-sends identical bytes with no new
  seal and no flash program; rearm only from the sent state.
- `test_sf5l0_bench_round_trip.cpp`: pacer gap/timeout/back-off, disable,
  32-bit wrap; 3 h at maximum rate never exceeds burst + 6 % in any sliding
  hour (below 10 %), long-run rate equals the duty rate.
- Full host suite PASS; default `rak4630` image unchanged in size.

## Physical result (2026-10-24, owner bench, `ad71a89`, remote over SSH)

Both boards re-flashed (the collar re-flash is its reboot). PC tool on the
Gateway port, ~20 minutes:

```
DUMP : VARDI seq=11005, VARDI seq=11089   (already in the ledger)
       YENİ  seq=11215                    (sent at collar boot)
live : YENİ  seq=11216 … 11245            (30 records, in order)
       YENİ  seq=11257 … 11265            (9 records, in order)
ÖZET : 40 new verified records, 0 rejected, 0 duplicates stored twice
```

Every record is the oldest retained one at its moment, AEAD-verified, decoded
(GNSS fix, 7–11 satellites, battery) and de-duplicated. No ACK was lost and no
retry was needed.

**The 11246–11256 gap is page-granular ring retention, not RF loss.** An
ObservationStore page holds 42 PERIODIC records; 11215–11256 was one page.
While the store is full, each new record that needs a fresh page erases the
oldest page whole. The drain had sent 31 of that page's 42 records when it was
erased, so the other 11 were overwritten. Draining a full store that keeps
recording at 1/min loses up to one page per page-roll unless the drain stays a
page ahead. Production intervals (15–60 min) make this moot; on the bench,
`APP INTERVAL 900` lets the drain catch up.

