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
  Built with ASan/UBSan.
- `test_sf5l4_range_kit.py`: RSSI pairing and clearing; first arrival and
  delay; DUMP does not overwrite; pre-SF5L4 ledger; page wiring; live-first
  enabled only inside `ORUN_SF5L0_TRACKER`; pilot default off.
- Headless Chromium: range view with 40 records (34 measured), desktop and
  phone, no page errors.
- Builds: `rak4630` unchanged at 359 584 B (local GCC 13);
  `rak4630_sf5l0_tracker_bench` 371 496 B; gateway bench and SF5K3V pilot
  build.

## Field procedure (owner)

1. Gateway at home on the PC: `--port … --serve 8080 --host <Tailscale IP>`.
2. Collar: `APP INTERVAL 60` for the walk, `BENCH DRAIN ON`.
3. Walk out with the phone (map over Tailscale, range view) until points turn
   red or stop, then walk back. The backlog recorded out of range arrives on
   return and is marked "geç".
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


## Not in scope

Production scheduler or QoS policy, RF parameter changes, the Gateway-side
hand-off, offline tiles, the ORUN app.
