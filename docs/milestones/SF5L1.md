# SF5L1 — bench Gateway hand-out over USB → verified local ledger

**Status:** code + host tests + **physical bench PASS (2026-10-23)**. Bench only (SF5L0 images).
**Base:** `main@7ce6a39` (SF5L0 merged, physical round trip PASS).
**Scope:** the next link after custody: what the Gateway durably holds reaches
a computer and becomes a readable, de-duplicated observation. The Tracker↔
Gateway ACK rule is unchanged: the Gateway ACKs after its own durable write,
and from then on the record is the Gateway's responsibility.

## What it adds

- `sf5l0ForEachHeld()` (firmware): read-only walk over OCQ2 records the
  Gateway holds, via the existing `ProductCustodyStore::readConfirmedHeld`,
  oldest page generation first. Never writes, hands off or deletes.
- Gateway bench image: after each durable custody + ACK, one USB line per new
  record; `BENCH GW DUMP` lists everything held (e.g. after a reboot):
  `EDGE HELD v1 gw=<16hex> page=<n> slot=<n> gen=<16hex> len=<84|100> obj=<hex>`
  The object is the exact opaque PRODUCT_SECURE frame; the Gateway does not
  decrypt it.
- `tools/orun_edge_bench.py`: reads those lines from the serial port (sends
  `BENCH GW DUMP` on connect) or a saved log, opens each frame with the
  existing `backend/candidates/product_secure_verified_ledger.py` (AES-CCM +
  canonical checks + SQLite dedupe by device/incarnation/sequence) and prints
  time, position, battery and activity. `--list` reprints the ledger. The only
  credential is the PUBLIC test key, and only with `--bench-test-key`.

## Not in scope

Gateway-side release/hand-off marking (needs an authenticated Edge accept,
SF5I direction), phone/BLE transport, backend server, map.

## Evidence

- `test_sf5l0_bench_round_trip.cpp`: held-record walk returns exactly the
  ACKed bytes; three records come back once each in order after a reboot;
  walking is read-only; empty region → none.
- `test_sf5l1_edge_bench.py`: the firmware line format parses; the on-device
  CC310 KAT frame opens and decodes; the same logical record re-sealed with
  another counter or re-sent is recognised as already present; a flipped bit
  or wrong length is refused; without `--bench-test-key` everything is refused.
- Default `rak4630` image unchanged in size.

## Physical result (2026-10-23, owner bench, `4380db3`)

First attempt found a real defect: the Adafruit core's `Print::printf`
formats into `char[256]`, so the ~280-char line was cut and the tool raised on
odd hex. Fixed in `4380db3` (object via `Serial.write`; tool refuses cut lines).

```
tool: [gateway port] dinleniyor
DUMP: YENİ tasma=0E8ADE7E71531AA3 seq=11005 zaman=2026-10-20T13:46:36Z
      konum=37.610723,28.054740 (uydu=8 hdop=1.07) pil=4116mV NORMAL
      EDGE DUMP END count=1
collar re-flashed remotely (= reboot) -> pilot sent its oldest record:
live: YENİ tasma=0E8ADE7E71531AA3 seq=11089 zaman=2026-10-20T15:10:36Z
      konum=37.610550,28.054666 (uydu=10 hdop=0.96) pil=4119mV NORMAL
```

seq 11005 is the record ACKed in the SF5L0 run (still held by the Gateway,
handed out on DUMP); seq 11089 arrived live through the whole chain
Tracker → Gateway custody + ACK → USB → AEAD open → ledger.

**Observation:** seq 11006–11088 were not lost in transit. The collar's
ObservationStore is full (1260 retained) and records every period; between
the two runs (~100 min) new records overwrote the oldest ones (ring
retention, `lost` counter). The one-record-per-boot pilot cannot keep up.
The real send scheduler (cutover step 3) must drain faster than records are
produced, or the oldest backlog is overwritten by design.

## Bench procedure

1. Close any serial monitor on the Gateway (one program per port).
2. `python3 tools/orun_edge_bench.py --bench-test-key --port <gateway port>`
   → lists every held record (`YENİ`/`VARDI`), then waits.
3. Reset the collar: its pilot sends the next stored record; a new `YENİ`
   line appears.
