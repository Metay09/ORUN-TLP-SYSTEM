# Phase 2 real-key Tracker USB commissioning (opt-in, NOT product release)

Target: new Tracker `52E16773141A8887`; current Gateway `09A462BD4B275BA5`.
Existing firmware, bootloader and product ObservationStore data are not erased by
the enrollment protocol. **Uploading any firmware needs a separate verified DFU
path; do not assume a firmware build alone proves flash retention.**

## What this implementation adds

- Explicit PlatformIO environment `rak4630_phase2_tracker_real_pilot`,
  not `default_envs`. Inherits the one-secure-object-per-boot RF pilot.
- `ORUN_STEP3A_REAL_TRACKER_PROVISION` is compiled only for that environment.
  No `ORUN_SF5L0_BENCH_TEST_KEYS` or public demo credential is included.
- `BENCH?` and `BENCH PROVISION ...` reuse the existing exact Step 3A
  CLI protocol for `tools/orun_provision.py tracker`, without bench keys. The
  "BENCH" text is legacy CLI compatibility, not a claim of public credentials.
- The password-equivalent 32-byte root is accepted **only over the physically
  connected USB serial port**; it is wiped from working buffers and never
  printed. The CRC is merely a line transfer guard, NOT authorization.
- Store may transition only from UNPROVISIONED to a new real per-device
  credential, or resume with the identical credential. A different, foreign,
  faulted or busy SecurityStore refuses. V3 migration precedes a single exact
  Gateway grant; no automatic erase, factory reset or credential replacement.
- `BENCH SETUP PASS` means local durable credential/V3 grant only. It does NOT
  mean Gateway has a matching signed OGK1 grant, did durable custody, or
  returned a verified ACK.
- `PRODUCT_SECURE` TX pilot may send a single sealed frame; its payload
  remains retained unless the authenticated exact-object ACK has durably
  released it.

## PC preflight (no device modification)

```sh
git -C "$HOME/ORUN-FAZ2-VERIFY" fetch origin feat/phase2-product-secure-integration-20261031
git -C "$HOME/ORUN-FAZ2-VERIFY" switch --detach FETCH_HEAD
cd "$HOME/ORUN-FAZ2-VERIFY/firmware"
pio run -e rak4630_phase2_tracker_real_pilot
pio run -e rak4630
```

Build/host PASS must be observed on the owner's PC before target upload.
No new firmware has been installed or commissioned by this PR alone.

## Still blocked for production

The existing bench Gateway has public-test-key fallback and may lack an
independent rollback-resistant grant revision anchor; the real pilot has
limited boot-time RF scheduling. Gateway-to-Edge authenticated accept, real
signed current OGK1 install, backend delivery, stress/power-cut qualification
and production release gate are not established by this change. Never
reflash/erase the currently running Gateway or interrupt PC Edge/Backend
to make the pilot appear successful.
