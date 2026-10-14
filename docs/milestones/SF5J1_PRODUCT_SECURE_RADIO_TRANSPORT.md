# SF5J1 — PRODUCT_SECURE radio-owner transport candidate

Baseline: `main@e7e99d1c33504da6ccfc8b236e7b82e5c0ae2f56`.
Branch: `feat/sf5j1-product-secure-radio-transport`.

## Change

The existing SX1262 `RadioManager` now has an isolated **transport-only**
PRODUCT_SECURE seam:

- TX admits an already-encrypted and encoded local-source PRODUCT_SECURE frame
  of exactly 84 bytes (EVENT) or 100 bytes (PERIODIC). It verifies structural
  header rules using the frozen SF5B codec and passes the exact frame to
  SX1262; it does not seal, allocate counters, or create packet identities.
- RX structurally validates a PRODUCT_SECURE header and copies the exact
  received bytes and RSSI/SNR into a single bounded loop-owned mailbox.
  Pending bytes are never overwritten: overflow and malformed frames increment
  dedicated diagnostic counters.
- No new commands, memory erase, custody ACK, tracker release, radio schedules,
  product service activation, or USB bridge protocol were introduced.
- Original POSITION, RELAY and HISTORY_SECURE paths remain intact.

**Scope warning:** visible PRODUCT_SECURE fields are not authenticated by
structural decode. `RX PRODUCT_SECURE ... unverified=yes` is not a security
acceptance, delivery, or durable-custody event. Only the backend verifies the
application AEAD; a future Gateway custody owner holds exact ciphertext
objects without acquiring the tracker's root key.

## No automatic RF activation in this slice

Both new RadioManager methods are intentionally uncalled by the production
composition root. A new product send-owner may call TX only after fulfilling
`docs/architecture/ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md` section 10:
durably cache one exact custody-requested protected object **before** its
first on-air transmission, retransmit byte-for-byte without re-sealing on
ordinary retry, and enforce the bound of four outstanding custody objects.

The target Gateway service must provide real accepted custody or verify an
authenticated exact-object `EDGE_DURABLE_ACCEPT` before it can consider a
tracker-facing custody ACK. This PR neither creates nor validates that service.
A gateway that receives PRODUCT_SECURE over this seam cannot acknowledge it
based on RX or `TX_DONE`. The one-frame mailbox is not the product custody
store. Product RF activation requires its own reviewed gate.

## Existing physical evidence (legacy path only)

2026-10-14: physical RAK4631 Tracker `0E8ADE7E71531AA3` sent legacy
POSITION packets 40318, 40319 and 40320, and BASE
`09A462BD4B275BA5` received and USB-bridged all three (RSSI -19/-20 dBm,
SNR 10 dB). This is an RF link sanity baseline **not** evidence that the new
PRODUCT_SECURE path is active or hardware-qualified.

The BASE product region showed `unknown=32`,
`decision=NEEDS_PREPARATION`. It must not be silently wiped or mounted as an
ObservationStore in place of Gateway custody.

## Verification commands for developer checkout (fish)

```fish
git fetch origin
git switch feat/sf5j1-product-secure-radio-transport
git pull --ff-only
bash firmware/tests/run_host_tests.sh
pio run -d firmware -e rak4630
git status --short --branch
```

Use `firmware/tests/sf5/test_sf5j_radio_transport.cpp` for the new fake
SX1262 driver tests: exact 100B RX copy, RSSI/SNR, bounded mailbox and
malformed-reserved-byte rejection; 84B TX and local device binding; invalid
profile rejection. The frame fixture intentionally has **no AEAD proof**;
this test only checks transport and structural validation. Maintain the
existing full host suite, ASan/UBSan checks, RAK4630 build and existing
production startup variants. This milestone is **pending local/CI execution**
until results are attached.

## Next bounded slices

1. Tracker durable exact-object lifecycle (SF5C control slots) and protected
   send scheduler with current POSITION retirement gated explicitly.
2. Gateway product flash owner/CustodyStore service and full exact-object
   intake; no global flash preparation merely to silence UNKNOWN pages.
3. Gateway-Edge exact-byte durable acceptance (SF5H/SF5I), authenticated
   tracker custody ACK under separate durable authority, backend receipt,
   and physical two-device failure/recovery tests.
