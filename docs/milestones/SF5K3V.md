# SF5K3V — Tracker PRODUCT_SECURE one-packet RF pilot

Base: `6ed4ee020c2e5b4e04facce598cf784a574abd3a` (SF5K3U).

One bounded, application-loop-owned **Tracker TX pilot**, NOT a product
cutover. The default `rak4630` image remains the existing POSITION +
HISTORY_SECURE runtime; new RF emission is compile-time **OFF**.

- `TrackerProductTxPilot` selects the oldest retained, non-uncertain PERIODIC
  from the actual OBS1 ProductRecordStore. It verifies the canonical plaintext
  includes the exact stored incarnation/sequence.
- It checks for existing flash-cached ciphertext **before** asking
  ProductSecureCrypto to seal. Missing frames are sealed once using the real
  SecurityStore nonce owner; `RELAY_ALLOWED | CUSTODY_REQUESTED` bits are
  fixed at creation, never switched on retries. Older direct-only cached
  frames are sent *as-is*, never reprotected.
- Flash cache submission is never a send permission. Only a durable cache
  write outcome and byte-identical readback permit `RadioManager` TX.
  Failed or uncertain writes block sending and preserve the logical record.
- **At most one TX admission per boot**; no autonomous burst or retry loop,
  no ACK-based early release, no flash reset/repartition. The existing
  SF5K3U HMAC + stored-object verifier remains the only release authority.
- Pilot RF is gated by a new explicit PlatformIO test environment
  `rak4630_sf5k3v_tracker_tx_pilot`; NEVER flash it merely because it
  compiles. Need a separately commissioned Gateway with a durable scoped
  grant, and an explicit hardware test plan.
- The fake-sealer + fake-radio host test exercises **real OBS1/record cache**
  (sync/async), cache-before-send, unchanged reboot retransmission, and
  flash failure safety. Fake crypto is **not** AEAD proof.
- No change to the 0x04 PRODUCT_SECURE or 0x05 56-byte custody ACK wire.
  No relay, batch ACK, Gateway flash activation, Edge handoff, RF receive
  windows, duty-cycle or energy measurements included.

Validation required on the new commit:
```sh
bash firmware/tests/run_host_tests.sh
pio run -d firmware -e rak4630
pio run -d firmware -e rak4630_sf5k3v_tracker_tx_pilot
```
Do not claim PASS before those commands actually pass.
