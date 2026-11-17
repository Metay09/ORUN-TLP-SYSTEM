# KQ05 — Gateway receipt staging: PC bridge and physical integration boundary

**Status:** DRAFT / HOST ONLY / NOT FIELD RELEASE. Source baseline: PR #330
(`651e8957e47071ad214bebb572d8899857faf0b4`). This is a single, deliberately
non-deployed step toward Issue #315. Do not merge as a physical-device release.

## Existing product invariants

- Ordinary `PERIODIC` LIVE and opportunistic HISTORY remain ACKless. A lost
  receipt cannot stop a newer position. The selective Backend-Durable HMAC
  receipt releases **only the exact proven PERIODIC identities** in OBS1.
- Critical `EVENT` uses existing Gateway custody ACK and exact ciphertext
  cache; Gateway custody is **not** Backend/App delivery.
- Edge->Gateway `STAGED` means only a **Gateway-local durable commit and
  independent readback**, not RF launch, Tracker acceptance or Backend deletion.
- A Gateway never possesses the Tracker root credential. It cannot validate
  or mint Backend receipt HMAC, nor authorize Tracker Flash release.

## What this host change integrates

`gateway/candidates/kq05_gateway_ble_durable_stage_bridge.py` connects:
1. Existing Backend verified SQLite ledger + selective batch issuer + outbox.
2. Existing authenticated Backend->Edge page and durable Edge SQLite inbox.
3. The exact 0x91 `edge_link` logical offer and its independently verified
   Edge/Gateway HMAC, source row, Gateway scope, Tracker scope, key epoch.
4. Existing bounded Gateway SQLite staging, WAL/FULL transaction, complete
   integrity scan and post-commit readback.
5. Existing authenticated exact-offer 83-byte Gateway result wrapped as the
   **84-byte** 0x92 logical message, checked by the Edge verifier.

The host test checks unavailable authorities, malformed/corrupted offers,
successful commit, duplicate after restart, second offer while storage is full,
tampered durable SQLite and the absence of fake STAGED responses. The Backend
outbox and Edge inbox must still contain their records afterward. No device,
RF transmitter, physical Flash region, live database or service is touched.

### PC gate

```bash
PYTHONDONTWRITEBYTECODE=1 python3 \
  firmware/tests/sf5/test_kq05_gateway_ble_durable_stage_bridge.py
```

Added to `tools/ci/run_kq05_lab_preflight.sh` after the original separate
Edge/Gateway durable-stage regression.

## Physical Gateway NO-GO (intentionally unchanged)

The RAK Gateway in `firmware/src/sf5l0_bench_gateway.cpp::edgeMessage()`
currently prints `KQ05 EDGE OFFER refused: durable Gateway owner not installed`.
**Keep this refusal until the real owner and authenticated Edge enrollment
exist.** A host SQLite candidate is NOT a replacement for RAK NOR.

The existing 128 KiB Product region is exclusively owned by the Gateway's
`ProductCustodyStore` with OCQ2 records; its pages cannot be silently
repurposed for new receipt records. The reserved geofence/security/config/bond
and legacy-history regions also have assigned owners. Adding physical durable
Gateway staging would require a reviewed non-overlapping partition, migration,
power-cut recovery and one FlashMutationGate owner.

`SignedGrantCredentials` requires a separate
`TrustedEdgeInstallation::readVerifiedInstallation()` implementation.
The issuer-signed OGK1 v2 snapshot carries a key, **not** a trusted installation
identity; a caller-provided BLE name is forbidden as a substitute.
Do not wire the existing `GatewayRamRelay` with mock credentials on hardware.

**Less intrusive physical alternative to evaluate:** Keep receipts durable at
Backend and Edge; relay a bounded authenticated offer through Gateway RAM
(with independent trusted installation enrollment), applying retry/expiry
without ever asserting `STAGED`. This avoids a second Gateway Flash owner
but still needs a real RF scheduler, regional duty budget and authorized
Tracker RX window. Do not turn RAM enqueue into a positive storage receipt.

## Remaining physical acceptance

- Bind independent, revocable, local Edge installation authority to signed
  OGK1 v2 Gateway grants. Reject absent/changed/revoked enrollment.
- Decide physical Gateway storage owner versus durable Edge + volatile Gateway
  relay; do not invent a flash partition without a migration plan.
- Connect actual BLE frame reassembly, bounded offer admission and durable
  retry; never create a positive STAGED from parser or RAM state.
- Give critical custody ACK and CONTROL first priority; only transmit a batch
  Backend receipt within a verified regional RF airtime budget and authorized
  Tracker RX window.
- Verify Tracker's own Backend MAC and exact OBS1 release markers; test
  1/2/4 collars, loss, reboot/power-cut, full storage and 24–48h offline.
- Retain Issue #221 BLOCKED until real-hardware, power and security gates pass.
