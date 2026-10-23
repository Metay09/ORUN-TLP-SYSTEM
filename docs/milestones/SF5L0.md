# SF5L0 — bench single-record round trip (PUBLIC test keys)

**Status:** code + host test + **first physical bench PASS (2026-10-23)**. Bench only.
**Base:** `main@2386041` (SF5K stack merged via #153).
**Direction:** step 1 of `ORUN_PRODUCT_RF_CUTOVER_CONVERGENCE.md` ("one
exact-object direct round trip"), owner decision 2026-10-23 to prove it on two
boards with public test keys before production enrollment exists.

## What it adds

Two opt-in PlatformIO images. The default `rak4630` image is byte-for-byte
unaffected in behavior and identical in size (all new code is behind
`ORUN_SF5L0_*`).

| Image | Board | Adds |
| --- | --- | --- |
| `rak4630_sf5l0_tracker_bench` | collar | SF5K3V one-record TX pilot + `BENCH SETUP <gateway id>`: public test credential → v2→v3 migration → one durable v3 Gateway grant, on the real `SecurityStore` through its existing private policy path. Pilot TX waits until a grant exists. |
| `rak4630_sf5l0_gateway_bench` | receiver | Existing SF5K3P..X owners wired to the radio: OCQ2 `ProductCustodyStore`, `GatewayLocalCustodyOwner`, `GatewayCustodyAuthorizedIngress`, `GatewayProductStartupGuard`, `GatewayProductCustodyRuntime`. Grant source `Sf5l0BenchGrantSource` derives the SF5B §12 scoped ACK key from the public test root. `ObservationStore` is not mounted. |

Unchanged: PRODUCT_SECURE 0x04 and custody ACK 0x05 wire bytes, ACK
verification and release owners, production enrollment (SF5K3Y..AC), TLP v1
and HISTORY_SECURE paths.

## Security boundary

The credential is the public SF3 test vector: anyone can derive every key.
A Gateway holding a Tracker root is forbidden in production; it is acceptable
here only because the root is public. `BENCH SETUP` refuses any device that
already holds a different credential. Never flash these images in the field.
Recovering a collar afterwards: `rak4630_m7p6f_security_cleanup`.

## Evidence

- `tools/ci/run_sf5l0_bench_round_trip_host.sh` (in `run_host_tests.sh`):
  blank real `SecurityStore` → `BENCH SETUP` sequence → v3 grant survives
  reboot; Gateway-signed ACK (real custody owner/signer/ingress, sync + async
  flash) authenticates in the real `CustodyAckVerifier`; tampered ACK/object,
  ungranted Gateway and wrong key epoch are refused; a foreign credential is
  never touched.
- Full host suite PASS; both bench images and `rak4630` build (local GCC 13
  toolchain; owner GCC 7.2 build still required).
- `test_sf5f_store_scope_contract.py` now judges the default composition with
  the `ORUN_SF5L0_GATEWAY` block removed.

## Physical result (2026-10-23, owner bench, `4e0c650`, GCC 7.2 build)

Gateway `09A462BD4B275BA5` (BASE, product region wiped with
`BENCH GW ERASE PRODUCT CONFIRM` → `READY`), collar `0E8ADE7E71531AA3`
(UNPROVISIONED, 1260 retained records), boards side by side (RSSI −15, SNR 9).

```
collar : BENCH SETUP 09A462BD4B275BA5 → CREDENTIAL → MIGRATE_V3 → GRANT → PASS
collar : PRODUCT PILOT exact cache=durable
collar : TX PRODUCT_SECURE id=0E8ADE7E71531AA3 family=1 length=100 counter=1
gateway: RX PRODUCT_SECURE queued family=1 length=100 rssi=-15 snr=9
gateway: BENCH GW RX record; durable custody write started
gateway: TX CUSTODY_ACK tracker=0E8ADE7E71531AA3 gateway=09A462BD4B275BA5 len=56
collar : PRODUCT CUSTODY ACK authenticated; release=pending
collar : PRODUCT CUSTODY release=DURABLE
```

Proves on hardware, for one record at bench range: seal + durable cache →
RF → Gateway durable OCQ2 custody → ACK only after custody → HMAC verified
against the real v3 grant → durable Tracker release. Not proven: range,
timing margin, relay, reboot/duplicate paths, power, Edge/backend.

## Bench procedure

1. Gateway board: flash `rak4630_sf5l0_gateway_bench`. Expect
   `BENCH GATEWAY id=<GW> custody_region=READY`. If `FOREIGN_REGION` /
   `NEEDS_PREPARATION`: `BENCH GW ERASE PRODUCT CONFIRM` (wipes this board's
   product region, reboots). `BENCH?` must show `role=BASE`.
2. Collar: flash `rak4630_sf5l0_tracker_bench`, tracking on, at least one
   stored PERIODIC (`PRODUCT STORE?` retained ≥ 1). `BENCH SETUP <GW>` →
   `BENCH SETUP PASS`.
3. Expected sequence: collar `PRODUCT PILOT TX=start` → gateway
   `RX PRODUCT_SECURE queued` / `BENCH GW RX record; durable custody write
   started` / `BENCH GW ACK TX started` / `TX CUSTODY_ACK` → collar
   `PRODUCT CUSTODY ACK authenticated` / `PRODUCT CUSTODY release=DURABLE`,
   and `PRODUCT STORE?` released count +1.
4. Reboot collar: the next retained record goes; a re-sent identical record is
   re-ACKed by the Gateway without a second flash write.

Not covered: relay path, 4-object windows, timing margins at range, duty
cycle, current, Edge/backend hand-off.
