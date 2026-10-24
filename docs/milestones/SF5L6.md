# SF5L6 — BLE Edge link (Gateway records over Bluetooth, accept over Bluetooth)

**Status:** code + host tests. **Physical bench check: pending (owner).**
Bench only for authentication (public test key, open BLE link).
**Base:** `main` with SF5L5 merged.

## Why

In the field the Edge (phone, or PC on the bench) walks with the Gateway and
cannot rely on a USB cable. SF5L5 proved confirmed delivery and reclaim over
USB. This milestone carries the same facts over BLE, so USB and BLE are two
transports for one hand-off protocol (MQTT/HTTPS to a backend comes later,
from the Edge, not from the Gateway).

Owner decisions:
- BLE and USB are both required; others (MQTT…) when needed.
- The Gateway never turns BLE off (`BleAdmissionPolicy::setAlwaysAvailable`).

## Link

A separate GATT service, next to the M7P7F application service (whose
48-byte logical limit cannot carry a 100-byte custody object):

| | UUID |
|---|---|
| ORUN Edge service | `6f9a1c20-5b3e-4d8a-9e61-2c4b7a0d3e51` |
| Edge → Gateway (WRITE) | `6f9a1c21-5b3e-4d8a-9e61-2c4b7a0d3e51` |
| Gateway → Edge (INDICATE) | `6f9a1c22-5b3e-4d8a-9e61-2c4b7a0d3e51` |

The service UUID is in the scan response, so an Edge finds the Gateway
without connecting.

**Framing** (`edge_link.h`, pure code, host-tested): 20-byte frames (ATT MTU
23, no MTU negotiation assumed): `version | flags (START=1, END=2) | message
id | fragment index | 16 data bytes`, logical message ≤ 160 bytes.
- Gateway → Edge is stop-and-wait: the next frame is indicated only after
  the previous indication was confirmed (HVC). BUSY/RESOURCES → retry in
  25 ms; any other error disconnects.
- Any gap, reorder, version or size violation drops the partial message;
  a partial message expires after 3 s. Partial messages are never used.

**Messages** (big endian):

| Type | Dir | Body |
|---|---|---|
| `0x01` HELLO | GW→E | gateway id(8) held(2) capacity(2) |
| `0x02` HELD | GW→E | page(2) slot(2) gen(8) flags(1) rssi(2) snr(1) len(1) object |
| `0x03` DUMP_END | GW→E | count(2) |
| `0x04` ACCEPT_RESULT | GW→E | page(2) slot(2) code(1): 0 handed off, 1 already, 2 unknown, 3 busy, 4 bad MAC, 5 malformed, 6 rejected, 7 failed |
| `0x05` RECLAIMED | GW→E | page(2) ok(1) |
| `0x81` DUMP | E→GW | (empty): start the full pass again |
| `0x82` ACCEPT | E→GW | page(2) slot(2) gen(8) mac(32) |

HELD flags bit0: rssi/snr are the Gateway's live RX of this record (the
range/coverage signal of SF5L4). The ACCEPT MAC is the SF5L5 MAC, unchanged.

## Gateway session (`Sf5l0EdgeLinkSession`)

On each connection: HELLO → every held record once (cursor walk over the
store) → DUMP_END → then new records as they are admitted. Accept results
and reclaim notices go first (6-entry queue). One accept is in flight at a
time, shared with the USB path (`Sf5l0EdgeHandoff`). BLE callbacks only copy
frames into an 8-slot ring; all work happens in `loop()`.

`BENCH?` third line: `BENCH GATEWAY edge_ble=ready|off session=… out=… in=…
dropped=…`.

## PC tool

`tools/orun_edge_bench.py --ble [ADDRESS]` (needs `bleak`): scans for the
service UUID (or uses ADDRESS), subscribes to indications, feeds the same
ledger as USB (`_ingest_held`), and writes the binary ACCEPT after the
ledger commit (write with response). On disconnect it reconnects every
5 s. `--no-accept`, `--serve`, `--host` work as with `--port`.

## Evidence

- `test_sf5l6_ble_edge.cpp` (ASan/UBSan): shared MAC vector + SF5L5
  hand-off/reclaim tests; framing (every size 1..160, shared frame vector,
  gap, foreign message id and bad version dropped, 3 s expiry); session over the real
  store (HELLO, every held record once, DUMP_END once, new records with
  link, accept → result, reclaim notice, reconnect restarts the pass).
- `test_sf5l6_ble_edge.py`: the same frame vector in Python; HELLO/HELD
  drive the ledger; binary ACCEPT equals the text accept's MAC; results,
  reclaim and DUMP_END update the queue and summary.
- Fake-`bleak` smoke run of `run_ble`: connect → HELLO → HELD → ACCEPT
  written in 20-byte frames → result → summary `teslim=1` → reconnect.
- Full host suite PASS (170). `rak4630` unchanged (362 912 B). Gateway bench
  385 536 B flash / 37 100 B RAM; Tracker bench 375 312 B.

## Known limits

- The BLE link is open (no pairing): anyone in range can read held records
  (still the Tracker's sealed objects) and send accepts; only an authentic
  MAC hands a record off. Edge identity and link security are step 3.
- One BLE central at a time: while the phone app uses the M7P7F service,
  the PC cannot connect.
- ~1 KB/s class throughput (stop-and-wait indications): a full 1056-record
  pass takes minutes. Faster MTU/connection settings can come later.
