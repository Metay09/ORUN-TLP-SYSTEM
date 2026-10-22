# SF5K3AA — RAK4630 pinned CryptoCell Ed25519 verifier pilot

**Base:** SF5K3Z `17ee958f5b7451cd78b17cdea1dd6133d4586571`.

## New engineering slice

- `GatewayEd25519Cc310Verifier` uses the Nordic CryptoCell
  `CRYS_ECEDW_Verify` detached Ed25519 API, matching the 32-byte raw public
  key and 64-byte detached signature used by the OGK1 candidate and host
  SF5K3Z OpenSSL verification. The authenticated bytes are **exactly 1824**
  canonical OGK1 payload bytes, not CRC, post-processing or a prehash.
- The pinned CC310 API takes a non-const message; the verifier first copies
  the canonical input to scratch and wipes both payload and work context
  after each attempt. Invalid key shape, null message, and wrong length
  fail closed. `CRYS_OK` is a verified signature **only**, not a complete
  Gateway authorization or protected revision checkpoint.
- The issuer public key is supplied to the verifier by its *trusted caller*;
  this slice deliberately supplies NO production trust root or provisioning
  entrypoint. A key from OGK1, radio, BLE or USB may NEVER be trusted merely
  because it was received.
- Compile-time flag `ORUN_SF5K3AA_TARGET_CRYPTO_PROBE` confines the verifier
  to a standalone `rak4630_sf5k3aa_ed25519_probe` environment. The default
  `rak4630` image's composition root does not instantiate it. The standalone
  probe uses a **public test** key/signature pair generated off-device and
  exercises valid Ed25519, wrong signature, wrong message, wrong public key,
  bad length/null inputs. It contains no RF, flash writes or real grant
  keys and must NEVER be deployed as product firmware.
- The focused host test compiles the exact same adapter against a
  `crys_ec_edw_api.h` header-shape stub. The stub's verification operation
  is backed by **real OpenSSL Ed25519**, so marshalled key/message/signature
  bytes and failure behavior are exercised. This is NOT proof of the real
  Nordic binary or cryptographic engine on the target; the standalone
  PlatformIO build and then an explicitly approved hardware KAT are needed.

## Required checks

```sh
bash tools/ci/run_sf5k3aa_ed25519_host.sh
bash firmware/tests/run_host_tests.sh
pio run -d firmware -e rak4630
pio run -d firmware -e rak4630_sf5k3aa_ed25519_probe
```

A build passes syntax/linking, not physical Ed25519 execution. No device
upload, radio transmission, flash format migration, OCQ2/OBS1 erase, Gateway
service activation or credentials installation is authorized.

## Remaining release blockers

Real trustworthy issuer public-key enrollment/pinning, a separate physical
rollback-resistant `GatewayGrantRevisionAnchor`, power-loss-safe Gateway
grant provisioning/rotation/revocation and security-region single-owner
lifecycle, Gateway RX spoofing/capacity protection, physical LoRa and Edge
durable handoff remain pending. None can be inferred from this crypto probe.
