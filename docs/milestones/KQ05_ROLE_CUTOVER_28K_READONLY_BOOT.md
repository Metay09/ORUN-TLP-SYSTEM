# KQ05: Device role-aware 28 KiB boot preflight (not a cutover)

**Date:** 2026-11-18  
**Status:** DRAFT / opt-in, disabled in `firmware/platformio.ini` / no physical Flash mutation  
**Parent:** PR #341 at `095ca41c70c2664e55cf3007df486627dc4052e7`, verified `PC PASS` by the device owner.

## Problem in current unified KQ05 runtime

Current `main.cpp` uses one firmware for Tracker and Gateway and resolves
`storage_role` from Product/Security evidence and saved intent at boot.
Under `ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE`:
- Tracker **does not mount** the legacy 28 KiB `HistoryStore`.
- Gateway **still mounts** the legacy `HistoryStore` and polls it.

That is the correct behavior for the old image, but means the new 39-page
128 + 28 KiB segmented owner must **never** be activated while the
Gateway's legacy History writer remains live. The existing 39-page
host bridge relies on simulated `exclusive_history_owner_stopped`
and `device_role_identity_bound` inputs. Those are not real runtime
proofs and MUST NOT be promoted as such.

## This bounded change

New *explicit* `ORUN_KQ05_SUPPLEMENTAL_28K_PREFLIGHT` opt-in branch
in the existing firmware composition:
- Fails compilation if used without both unified Tracker/Gateway role
  macros and the KQ05 experimental test image macro.
- **Not set in any PlatformIO environment.** Normal firmware and
  prior role-dependent legacy mounts/polls are unchanged.
- Only **after** `decideBootStorageRole()`, takes the resolved actual
  device role and local `device_identity.legacyUint64()`.
- Calls `storage_flash_gate.begin()` (only initializes the read-only
  synchronous History port; it does not call `HistoryStore::begin()`)
  then `kq05_supplemental::inspectReadOnly()` for all seven history
  pages; logs `BLANK_CANDIDATE`, `TRACKER_CANDIDATE`,
  `GATEWAY_NEEDS_BINDING`, `LEGACY_OR_FOREIGN`,
  `CORRUPT_OR_INCOMPLETE`, `READ_FAILED` or `BLOCKED_ROLE`.
- Under this opt-in only, **never** calls `history.begin()` for Gateway
  or `history.poll()` for either role. A blank page therefore cannot
  receive the legacy HistoryStore's first-page initialization write.
- In every case, prints `WRITER=DISABLED; NO_MIGRATION`. It does
  **not** open the proposed new 39-page runtime writer and never
  erases the legacy History pages or changes a stored credential.
- A blank 28 KiB region is **not sufficient** approval to activate
  the supplemental owner. The real cutover must establish durable
  role/device identity binding, previous-owner liability discharge,
  exclusive writer shutdown and crash-safe migration.

### Host test

`PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_kq05_role_cutover_readonly_source.py`

The test checks explicit feature gating, that the diagnostic never
mounts/mutates legacy Flash, the unchanged normal Gateway path and
the opt-in poll exclusion. It is also added to KQ05 LAB preflight.
**Neither this source check nor the earlier fake Nordic SVC test
constitutes a real-device Flash ownership trial or a firmware build.**

## Remaining work for release

1. Durable proof that the old Gateway HistoryStore has no outstanding
   custody/observation liabilities, and an **operator-controlled** one-time
   migration (no automatic erase or background cleanup).
2. Authenticated, persistent per-device binding for the old History region,
   including Gateway OCQ2 header's missing Gateway identity binding.
3. Target build/stack/SoftDevice/low-power qualification of 39-page
   ObservationStore and ProductCustodyStore and actual shared physical
   mutation arbitration under multiple concurrent clients.
4. Properly versioned, per-Tracker scoped **Gateway-signed selective
   batch custody ACK** for routine observations. Do not conflate with
   Backend durable receipt. Test offline, multi-Gateway and power cuts.
5. Controlled hardware pilot and separate product go/no-go gate.

**No field-release claim. P0 remains BLOCKED.**
