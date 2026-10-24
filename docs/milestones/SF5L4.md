# SF5L4 — range-test kit (live first, signal per record, coverage map)

**Status:** code + host tests + headless-browser render check. **Field
range walk: pending (owner).** Bench only (SF5L0 images, public test key).
**Base:** SF5L3 (`feat/sf5l3-edge-map`, PR #159).
**Scope:** make one walk with the collar answer three questions: how far
does the link reach, how strong is it at each point, and does the newest
position reach the PC quickly even while a backlog is queued.

## Changes

### Collar (`rak4630_sf5l0_tracker_bench` only)

- `TrackerProductTxPilot::setLiveFirst(bool)`, **default off** (the SF5K3V
  image and every non-bench image keep oldest-first).
  - When on, each selection first tries the **newest** retained record, if it
    is a usable unreleased PERIODIC and either already cached or a cache slot
    is free. Otherwise it falls back to the oldest retained record.
  - If the four-object exact cache is full, the **newest cached** object is
    re-sent byte-for-byte (no new seal, no flash write) instead of blocking.
    No custody object is ever evicted.
  - This changes TX order only. Ring retention (oldest page erased when the
    store is full), release ownership and the ACK/HMAC path are unchanged.
    The exact-object cache count already excludes released and erased
    records, so slots free themselves on ACK and on ring erase.
- `setLiveOnly(bool)` (default off) and `BENCH DRAIN LIVE`, for range walks:
  - only the newest record is sent, right after it is stored, so its radio
    facts belong to the place where it was recorded;
  - nothing is released or skipped; the backlog waits and drains with
    `BENCH DRAIN ON`, which returns to live-first;
  - with four unanswered cached objects (out of range), the newest cached one
    is re-sent, so returning into range frees a slot instead of stalling.
  - `BENCH DRAIN?` reports `ON`, `LIVE` or `OFF`.
- No live legacy v1 POSITION TX while a custody ACK is awaited (bench image
  only). Measured before the fix: 6 of 29 ACKs were lost, each time a v1
  POSITION TX had started inside the ACK window. The fix is still stored as
  before; only its live v1 copy is skipped.
- USB lines:
  - `BENCH LINK tx seq=N newest=M LIVE|BACKLOG`
  - `BENCH LINK ack seq=N rssi=.. snr=.. rtt_ms=..`: the ACK as heard by the
    collar, TX start → authenticated ACK, printed after durable release.
  - `BENCH LINK ack_timeout seq=N`

### PC tool (`tools/orun_edge_bench.py`)

- Pairs the Gateway's `RX PRODUCT_SECURE queued … rssi= snr=` line with the
  record whose custody write it started, i.e. the RX line, then
  `durable custody write started`, then `EDGE HELD`. A duplicate or denied RX
  clears the pair, so a DUMP line never inherits another frame's signal.
- New table `edge_receipts` (device, incarnation, sequence, first PC arrival,
  gateway RSSI/SNR). The first arrival wins; a later DUMP is not an arrival.
- `/api/points` adds `rssi`, `snr`, `received`, `delay_s` (PC arrival − record
  time), `period_s` and the PC clock `now`. Ledgers from before SF5L4 still
  map, with these fields set to null.

### Map (`tools/edge_map/index.html`)

- **Menzil görünümü** (range view): points coloured by gateway RSSI (≥ −105,
  −105…−115, −115…−125, < −125 dBm; grey = no measurement), with a legend,
  the count of measured records and the weakest one.
- **Kapsama** (coverage grid), the owner's design with the radio caveats:
  - cells of 3 / 5 / 10 / 20 m, **default 5 m**, remembered per browser;
  - only good fixes count (≥ 5 satellites and HDOP ≤ 4);
  - each record's link is classified **by the tool** (`classify_links`,
    host-tested), not by the page:
    - **live**: gateway RSSI/SNR paired and at the PC ≤ 30 s after capture;
      only these supply a signal value (sent from about where recorded);
    - **through**: captured while the tool was attached to the Gateway
      (`edge_sessions`, heartbeat every 15 s) and at the PC ≤ 180 s;
    - **cut**: captured during such a session, at the PC > 180 s, and no
      record of the same collar captured within ±150 s arrived live;
    - otherwise **unknown** and not counted. This covers a record that only
      waited in the collar's queue while others arrived live, and one
      captured while nobody listened (e.g. the 20 October backlog).
    - First owner view (`21ef09d`) painted the 20 October backlog as Kopuk
      because it used delay alone; fixed here. Conservative edge: the first
      ~2 records after leaving range sit within ±150 s of the last live one
      and stay unknown.
  - the cell colour is **Kopuk** when fewer than half its records got
    through; otherwise it follows the median SNR margin over the SF11 floor
    (−17.5 dB): ≥ 15 / 8–15 / 3–8 / < 3 dB. The median is used, not the mean,
    so one outlier cannot move a cell. The popup gives the through-ratio, the
    margin, the median RSSI and the counts.
  - Spatial resolution is limited by sample spacing, not by cell size: SF11
    airtime and the duty budget allow one sample every ~25–40 s (≈ 35–55 m
    while walking). A 5 m grid fills by stopping, or by walking an area more
    than once.
- View switch İz / Menzil / Kapsama (remembered per browser).
- The tracker card shows the **age of the newest position** against the
  collar's own period, using the PC clock: green / amber / red with "ESKİ".
  The big marker stays the newest **by record time**, never the latest
  arrival. Late records show "geç · <delay>" in the list and popup.

## Evidence

- `test_sf5k3v_tracker_pilot.cpp` (`liveFirst`):
  - newest sent first;
  - unanswered newest re-sent as the same bytes;
  - a newer record overtakes;
  - full cache → newest cached re-sent, no seal and no flash program, while
    the default pilot still blocks;
  - after a release the next newest goes first;
  - with the newest released, the backlog drains oldest-first.
  `liveOnly`:
  - newest only;
  - a full cache re-sends the newest cached object, with no seal or flash
    write;
  - after a release the next newest goes;
  - with the newest released, nothing is sent and the backlog stays retained;
  - switching to live-first drains from the oldest.
  Built with ASan/UBSan.
- `test_sf5l4_range_kit.py`: RSSI pairing and clearing; first arrival and
  delay; DUMP does not overwrite; pre-SF5L4 ledger; page wiring; live-first
  enabled only inside `ORUN_SF5L0_TRACKER`; pilot default off.
- Headless Chromium: range view with 40 records (34 measured); coverage grid
  of a synthetic walk out of range, rendering green → yellow → orange →
  Kopuk cells and redrawing for 20 m; desktop and phone, no page errors.
- Builds: `rak4630` unchanged at 359 584 B (local GCC 13);
  `rak4630_sf5l0_tracker_bench` 371 816 B; SF5K3V pilot 365 400 B (the
  disabled live code; it stays oldest-first); gateway bench builds.

## Field procedure (owner)

1. Gateway at home on the PC: `--port … --serve 8080 --host <Tailscale IP>`.
2. Collar: `APP INTERVAL 60` and `BENCH DRAIN LIVE` for the walk.
3. Walk out with the phone (map over Tailscale, Kapsama view) and stop for a
   minute or two at places of interest, until cells turn Kopuk; then walk
   back. At home, `BENCH DRAIN ON`: the records captured out of range arrive
   as "geç" and confirm the Kopuk cells.
4. Optional: a laptop on the collar's USB records `BENCH LINK` lines (the
   ACK's strength on the way back, and timeouts).

Physical results go below once measured.

## Owner bench check (2026-10-24, `5cb953b`)

Collar `0E8ADE7E71531AA3` flashed with the tracker bench image; the Gateway
image was unchanged. The collar had 1001 retained records.
- The first TX after boot was the newest record (`seq=12594 … LIVE`), then
  the backlog continued oldest-first (`11592`, `11593` … `BACKLOG`).
- `BENCH LINK ack … rssi=-7 snr=9 rtt_ms=6654…6909` with the devices side by
  side. The RTT (TX ~2.3 s + Gateway durable write + ACK) leaves ~3 s of the
  10 s RX window.
- Phone map over Tailscale: newest #12602 shown **51 s** after capture,
  "Son konum 1 dk önce" in green, −6 dBm.

Observations:
- `BENCH GW ERASE HELD` was run before the PC had pulled the last ~13
  records. Two were recovered from the terminal and **6 were lost** (the
  collar had already released them). The manual erase has no PC
  confirmation; the real Gateway→Edge hand-off must not delete until the
  Edge confirms.
- Backlog drain at the 6 % budget (~94/h) against 60/h production at
  `APP INTERVAL 60` reduces the backlog by only ~34/h.
- The newest fix shown had 4 satellites and HDOP 7.2, about 30 m off the
  stationary cluster. A low-quality newest fix should be marked as such.


## Finding: collar-side ACK time grows with control records

Owner logs show `rtt_ms` rising ~255 ms per send, from 5.3 s to 8.9 s
(5.8 → 7.1 s in an earlier run), and dropping back later. Host measurement
on the real `ProductRecordStore` (FakeFlash read counts per
`snapshotCustodyCandidates`, which the ACK submit path calls) shows the same
shape: 127 → 3271 reads, about +100 per send/release cycle, falling back when
the control page rotates (about every 30 cycles). `findCachedExact` stays
around 100–142 reads. The same control scans run in the pilot's selection
and pre-TX checks.

Effect: the over-the-air ACK is not lost (0 timeouts once the v1 TX was
held), but `rtt_ms` overstates the radio time and the collar's loop spends
seconds in flash scans per ACK. This matters for power and loop latency in
production. The bench line now splits it:
`rtt_ms` (TX start → ACK taken from the radio), `verify_ms` (HMAC + exact
match) and `release_ms` (durable release). Owner device check (14:24): `rtt_ms` 3843–3851 steady, `verify_ms`
5367 → 6408 (+~260 ms per ACK).

Fixed separately in PR #161 (`perf/sf5-store-single-scan`, merged into this
branch for the bench image): one control-page pass, one data-page pass per
snapshot/count, and a table CRC-32. The pilot also stops re-scanning the
store on every loop pass while it waits for a new record (LIVE mode idle).

## Lessons carried to the ORUN app (owner asked to keep these)

- "Where is it now" is the newest position **by record time**, never the
  latest arrival; late records only extend history; show the age of the
  newest position.
- Arrival delay is not evidence of no radio link: it can be queue wait or
  nobody listening. Coverage needs listening-period evidence and live
  arrivals around the capture time.
- A radio measurement belongs to where the frame was **sent**; a backlog
  record's signal says nothing about where it was recorded.
- Coverage is shown as grid cells (owner: 5 m default) with the median of
  overlapping measurements and the through-ratio, using only good fixes.
- Deleting held records must wait for the next hop's confirmation (the
  manual `ERASE HELD` lost 6 records).

## Not in scope

Production scheduler or QoS policy, RF parameter changes, the Gateway-side
hand-off, offline tiles, the ORUN app.
