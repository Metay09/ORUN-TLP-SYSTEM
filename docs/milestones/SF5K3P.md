# SF5K3P — non-activated PRODUCT_SECURE Gateway local custody

**Status: focused portable host PASS; target RAK4630 build NOT RUN; physical flash/DFU NOT RUN.**

Based on SF5K3N (separate Gateway ACK signer). The earlier SF5K3O
`OCQ2` v3 84/100-byte format is included in this development slice.

## Implemented

- `ProductCustodyStore` uses the existing `FlashBackend` abstraction and the
  owner-approved single product flash region; **no new flash partition**.
- Conservatively mounts only erased or exact `OCQ2` v3 pages. Unknown,
  old `OCQ1`, corrupt, torn and unsupported regions are rejected without erase.
- One-way page setup and exact object append: body, commit marker, exact
  readback/CRC before announcing durable completion. Supports both sync and
  asynchronous `FlashBackend::kPending` via `pollPending`.
- Duplicate exact-object lookup uses full bytes, not a short hash hint.
- No automatic eviction, overwrite, page reclaim, or Edge handoff yet.
  Capacity exhaustion returns `kNoCapacity`; **never** fabricates custody.
- `GatewayLocalCustodyOwner` calls SF5K3N signer only after a positive exact
  flash readback on a committed held record, including reboot-safe duplicate.
  No signature is produced on pending, failed, corrupt or uncertain storage.
- Signer key is the already-derived per Tracker/Gateway/grant scoped key,
  supplied by a future trusted authority, not the Tracker `K_root`.

## Local evidence

- SF5K3O portable format host: PASS.
- SF5K3P fake-NOR synchronous/asynchronous, no early durable result,
  reboot at 5 write boundaries, duplicate, full capacity, corrupted record,
  partial flash failure and unreconciled mutation: PASS.
- SF5K3P owner ordering uses **test-only dummy signer**, to assert no signing
  before physical durability; this is **not** a real HMAC integration test.
- Production SF5K3N HMAC test is separate, not rerun here.
- Full repository host regression and real RAK4630 PlatformIO build: NOT RUN.

## Explicit blockers before RF/product activation

1. Operator-visible product-region ownership/preparation and `OCQ2` census,
   compatible with `OCQ1` detection; no silent wipe or migration.
2. Durable Gateway enrollment and *provisioning* of `K_custody_ack`, per
   tracker, Gateway, floor and grant generation. Source admission/rate/wear
   controls needed before accepting opaque, not-decryptable PRODUCT_SECURE.
3. Real LoRa PRODUCT_SECURE RX and Gateway owner composition; only after
   readback may `GATEWAY_CUSTODY_ACK` be queued, with bounded RX/ACK airtime.
4. Tracker PRODUCT_SECURE send-owner activation, secure ACK RX dispatch,
   `CustodyAckReleaseOwner` and grant/revocation serialization.
5. Gateway-to-Edge durable handoff, verified `EDGE_DURABLE_ACCEPT`, bounded
   capacity reclamation, physical power cut, reset and multi-node RF testing.

Never claim that `BASE` automatically provides custody or is a fully equipped
Gateway. No firmware upload, source merge, backend delivery or release in this slice.
