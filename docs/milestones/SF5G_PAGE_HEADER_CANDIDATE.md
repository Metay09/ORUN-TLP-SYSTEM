# SF5G — Next host-only page header and reclaim-intent candidate (2026-10-13)

Status: **CANDIDATE / HOST TESTS PENDING / NO DEVICE OR RUNTIME ACTIVATION**.

Parent: `main@50f6a2878dba8c0eaaf6bffcf437fbb1ce8a081e`
(PR #119, independently reviewed host-only record preflight).
This branch follows that merge without modifying production owners.

## Exact scope

* `firmware/tests/sf5/sf5g_page_header_candidate.h/.cpp`: new distinct
  magic `OCP3` / `0x4F435033`, version 3, and staged page-header /
  reclaim-intent **byte encoders and parsers** only.
* `firmware/tests/sf5/test_sf5g_page_header_candidate.cpp`: host tests
  for static layout, empty/staged/prepared/active states, corrupt CRC,
  page/version refusal, partial commit and activation, intact generation,
  intent committed/completed/torn states, and partial prefix/bit tears.
* Host runner compiles using `portable_flags` with the default ASan/UBSan,
  warnings-as-errors. This code is **not** referenced by production sources.
* Closes the one nonblocking PR #119 LOW test-strength issue using a positive
  exact-byte comparison and genuinely negative evidence/short-length cases.

This does **not** introduce FlashBackend IO, begin/mount, concurrent mutation,
erase permission, page rotation, dedupe, FIFO custody admission, durable
readback, Gateway identity/authorization, authenticated ACK, or any physical
power-cut evidence. An `kActive` parsed header **alone** is not permission
to mount/erase a page or issue custody ACK.

## Proposed layout (unfrozen)

The prior PR #119 4 KiB page geometry is maintained:

| Element | Size | Byte offsets |
| --- | ---: | --- |
| Page header | 64 B | 0–63 |
| Records | 32 × 120 B | 64–3903 |
| Permanently erased guard gap | 120 B | 3904–4023 |
| Reclaim intents | 3 × 24 B | 4024–4095 |

Page-header offsets:

* 0–3: `OCP3` magic; 4–5: v3; 6–7: v3 one's complement.
* 8–15: big-endian nonzero generation (not UINT64_MAX).
* 16–27: BE geometry [header, record, records/page, max object,
  intent size, intents/page].
* 28–31: CRC32 over bytes 0–27.
* 32–35: header commit marker, zero only after verified static bytes.
* 36–39: active marker, zero only after the commit.
* 40–63: must remain erased `0xFF`.

Reclaim intent (24 B): target page u16, reserved u16=0,
target generation u64, CRC32 over first 12 B, commit u32,
completion u32. The on-flash slot model and intent-exhaustion recovery
are **not approved**; separate storage state machine required.

No writes to existing SF4B `OCQ1` / v2 or SF5C `OBS1` media.
The parser classifies every non-OCP3 magic as `kForeign`. This provides
a candidate **refusal signal**; a future full product-region census,
mount decision, and non-destructive owner migration refusal remain mandatory.

## Unresolved before any durable Gateway deployment

1. Independently review new header/intent magic, layout, atomicity and
   unrecognized/torn-header recovery (including erased/unknown page policy).
2. Implement and separately test a FlashBackend state machine: stage,
   readback, commit, readback, activate; zero ACK before verified durability.
3. Close SF4B N1–N5 including exhausted intents and handoff markers,
   async `begin()`, read-fault propagation and scan/ACK timing.
4. Test every partial program/erase and reset boundary, foreign format
   preservation, FIFO head-of-line progress, duplicate exact ciphertext,
   spare pages and wear; measure target constraints.
5. Complete full custody-eligible family policy including delegated RESULT,
   tracker exact-ciphertext retention, ownership of roaming Gateway,
   and authenticated exact-object ACK. All are explicitly out of scope here.

Validation evidence for this new branch must be gathered *after* the
changes: an earlier PR #119 PASS does not qualify the new codec.
