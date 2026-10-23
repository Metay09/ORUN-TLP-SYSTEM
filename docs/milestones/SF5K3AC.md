# SF5K3AC — signed Gateway A/B append transaction SIMULATION

**Baseline:** SF5K3AB `bb5354275d69f9eaa87a4688f87c8765ddd956fc`.

## Exact scope

This milestone adds a **HOST-TEST-ONLY** staged A/B signed grant write
transaction, and tightens SF5K3AB's signed proposal preflight to require
the **exact same trusted issuer-verifier instance** for existing and
proposed OGK1 snapshots. Signature validity under a different injected
verifier is NOT sufficient to mint new Tracker ACK keys.

`GatewaySignedGrantAppendTransaction` is entirely guarded by the
`ORUN_SF5K3AC_TEST_ONLY_TRANSACTION` compile flag (used ONLY in the
focused host runner). The normal `rak4630` and Ed25519 device probe
builds do not instantiate it. No firmware-to-device upload is included.

The transaction requires all of:

1. Two already-verified OGK1 snapshots for the same nonzero Gateway,
   the SAME trusted issuer verifier object, and a fully valid append-only
   proposal preserving all existing Tracker scope/key bytes.
2. An external trusted **independent** exact revision anchor matching
   the active signed source and the proposal's next generation. The
   test uses a FAKE anchor, NOT a hardware anti-rollback guarantee.
3. Exclusive Gateway ownership of the shared two-page security region;
   explicit RF/ACK quiescence; the actual source reader bound to the
   exact same flash instance as the candidate writer.
4. Readback of the actual active-page bytes against the issuer-verified
   snapshot before ANY mutation. Then invalidate the live source
   reader for the rest of this boot to prevent stale custody ACK.
5. Erase ONLY the inactive page, read-verify it fully erased, write the
   signed proposal in <=64-byte aligned chunks with readback, and
   verify all staged bytes, signed proof and old active page again.
6. Commit only the final 4-byte zero marker after a last-check of the
   independent revision anchor. The source page is NEVER erased.
   An async accepted mutation is polled; it is never resubmitted.
   Late ambiguity or any mismatch latches permanent fault.

A successful physical simulated flash commit is deliberately returned
as `kCommittedAwaitingAnchor`. It is NOT authorized Tracker custody.
Until an independent, reviewed, rollback-resistant authority
advances the exact revision and the Gateway starts a new read-verified
session, the signed new page and the trusted checkpoint differ.
**Gateway MUST stay disabled.** No checkpoint writer is implemented.

## Power-cut behavior (important)

- Before beginning: no changes to old flash or active grant.
- After destination erase but before any prefix write: the old signed
  page remains. Booting with old revision can still recognize it.
- If power fails partway through the inactive page, the current
  strict OGK1 reader rejects the torn page and denies Gateway operation.
  This preserves *safety*, **NOT availability**. A reviewed recovery
  flow is still required; auto-erasing a torn page is forbidden.
- If commit succeeds but the independent checkpoint is still old,
  the signed newer page wins the flash scan but the authorization gate
  rejects it. This is again fail-closed until trusted reconciliation.
- If asynchronous flash completion has an uncertain outcome, there is
  no automatic retry and the Gateway cannot grant custody.

## Host tests (real OpenSSL issuer signing, FAKE monotonic authority)

```bash
bash tools/ci/run_sf5k3ac_signed_append_transaction_host.sh
bash firmware/tests/run_host_tests.sh
pio run -d firmware -e rak4630
pio run -d firmware -e rak4630_sf5k3aa_ed25519_probe
```

Tests cover clean commit, source-page preservation, lack of authority
before independent revision advancement, false issuer/exclusive-owner
denial before mutation, torn-prefix reboot refusal, body write fault,
anchor change before commit, and simulated asynchronous completion
without resubmission. This is *not* physical power-cut qualification.

## Blocking release requirements

A hardware-backed/independently authenticated exact revision anchor,
secure trusted issuer pinning, initial provisioning ceremony and key
confidentiality, safe recovery/roll-forward for torn A/B update, actual
device Ed25519 KAT, service-owner selection, protected writer controls,
and real LoRa Tracker-Gateway-ACK roundtrip remain OPEN.

**DO NOT** enable this test writer in a production environment. This
milestone does not prove Gateway deployment readiness. Flash/RAM
figures for normal builds do not include inactive Gateway components.
