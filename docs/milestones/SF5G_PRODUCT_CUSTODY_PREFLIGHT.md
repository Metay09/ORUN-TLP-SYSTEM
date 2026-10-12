# SF5G product-custody preflight — 2026-10-13

Status: **HOST-ONLY DESIGN MODEL + C++ RECORD-CODEC CANDIDATE, NOT A STORAGE-FORMAT APPROVAL OR GATEWAY ACTIVATION.**
Baseline: `main@b1d2239fa3f1a7c9f7f81f109b6a245017527a66` after SF5F step 1 scoped collar PASS.
Related contracts: `docs/milestones/SF4B.md`, `docs/milestones/SF5.md`,
`docs/architecture/ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md` (sections 3, 10, 12),
`firmware/include/product_region.h`.

## 1. Why a new version is required

* `custody_store_format.h` SF4B v2 is **OCQ1, 73-byte HISTORY_SECURE
  object, 92-byte record, 43 records/page**. It is portable-only and not a
  production Gateway storage owner.
* The SF5B PRODUCT_SECURE envelope has **100-byte PERIODIC** and **84-byte
  EVENT** protected frames, as pinned by `tlp_v2_product_secure.h`.
* Neither type fits the 73-byte SF4B slot. **Do not widen the old struct,
  reinterpret OCQ1 pages, reuse the 43/page estimate, or activate CustodyStore
  in normal firmware on the strength of this preflight.**
* SF5F step 1 durably stores **plaintext PERIODIC** records on the tracker.
  That is not the required persistent cache of the **exact protected bytes**
  before first custody-requested RF transmission.

## 2. Candidate Gateway page layout (not frozen)

This is a bounded, readable starting point for independent storage-format
review. All offsets below are **candidate**, not implementation constants.

```text
4,096-byte flash page
   64   page header (new distinct magic + version; NEVER OCQ1/v2)
3,840   32 x 120-byte fixed-size product custody records
  120   permanently erased record/intent separation
   72   3 x 24-byte reclaim-intent slots
------
4,096
```

Candidate 120-byte record:

| Offset | Bytes | Candidate field |
|---:|---:|---|
| 0 | 2 | Exact-object length, big-endian: 84 or 100 |
| 2 | 2 | Reserved = 0 |
| 4 | 100 | Exact protected object bytes; unused tail stays 0xFF |
| 104 | 4 | CRC32 over bytes 0..103 (corruption detection, not authentication) |
| 108 | 4 | Record commit marker, programmed last after readback |
| 112 | 4 | Edge-handoff marker A |
| 116 | 4 | Edge-handoff marker B |

The `32 x 32 = 1,024` theoretical slots across 32 product-region pages
are **NOT** usable queue capacity: a spare page, reclamation,
partially programmed slots, HELD records, intentional admission limits and
wear/maintenance reduce actual headroom. No capacity or battery-life claim
is authorized by this arithmetic.

Additional SF5 protected object families (including delegated RESULT) are
**out of scope** for this 84/100-byte candidate. Check their exact maximum
size and decide one interoperable storage representation **before** any
Gateway product flash format is approved.

## 3. Mandatory sequencing / failure rules

1. **Tracker**: build the SF5B protected frame with `CUSTODY_REQUESTED=1`
   using a fresh D2A security counter; persist **exact bytes**, bound to
   the logical ObservationStore identity, before the first RF transmission.
   Recovery and retry reuse those exact bytes. The existing SF5B limit
   is at most **four** outstanding protected custody-request objects per
   tracker, across relevant families.
2. **Gateway**: custody eligibility requires the **exact inner protected
   object** to be programmed, read back, committed last and reclassified as
   durable. A receive callback, RF `TX_DONE`, an enqueued mutation or CRC
   alone is never custody evidence.
3. **ACK**: only a separately authenticated `GATEWAY_CUSTODY_ACK` bound to
   the exact protected object, after local durable custody or validated Edge
   durable acceptance. No speculative ACK when flash is full, faulted, busy,
   still staging, or the record is only present in RAM.
4. **Selective release**: tracker ObservationStore releases only after
   authenticated ACK; unknown, stale, duplicate or mismatching ACK must
   not release any record. Suppress Gateway duplicates on full exact bytes,
   not merely the visible device ID or an untrusted truncated hash.
5. **Flash ownership (D2)**: one owner for the whole 128 KiB product region.
   Gateway job => CustodyStore; standalone tracker => ObservationStore;
   forwarding Relay alone is not Gateway. A roaming Gateway is still a
   Gateway job. Never erase or remount foreign/unknown pages implicitly.
6. **Edge write-around**: do not enable until the measured worst-case
   Edge-wait + local fallback commit/readback + ACK airtime fits the
   tracker's ACK rendezvous window. Otherwise use immediate local custody.

## 4. Open gates — must pass before runtime/flash activation

* Define and independently review a **new page magic/version**, full flash
  coding (CRC/readback/commit, page transitions, intents, tear states,
  recovery and migration refusal), acceptance rules and lifecycle.
* Close SF4B pre-runtime findings **N1–N5**: exhausted intent-slot recovery,
  exhausted double handoff-marker head-of-line behavior, pending-async
  `begin()` handling, read-failure propagation and measured scan/ACK timing.
* Prove full/partial page failures, abort/reset at every mutation boundary,
  deduplication after restart, zero ACK before durability, no unauthorized
  erase, queue-full refusal, wear and FIFO progress on the **new** format.
* Design the tracker's persistent exact-ciphertext cache and identity mapping
  without replacing its validated ObservationStore with a second full history.
* Resolve representation/size for *all* custody-eligible product families,
  actual target RAM/flash/airtime, authenticated ACK and Gateway/Edge
  trust-boundary behavior.
* Only after portable host/audit gates: production integration + build,
  bounded physical CustodyStore experiments with explicit safe storage
  ownership, and finally full tracker→Gateway→Edge evidence.

## 5. First branch scope and host-only evidence

`test_sf5g_product_custody_preflight.py` guards the frozen SF4B/SF5B
constants and checks the proposed page geometry, exact lengths, CRC and
commit model.

The separate **host-only** `sf5g_custody_record_candidate.h/.cpp` implements
staged 120-byte record encoding and classification, exact-object comparison,
CRC corruption checks, two handoff-marker states and structural PRODUCT_SECURE
84/100-byte shape validation. It is kept in `firmware/tests/sf5/` and linked
**only** into the dedicated `test_sf5g_custody_record_candidate.cpp` host
test. The latter exercises staged/partial-commit classification, 108 possible
prefix tears per record length, synthetic reboot/re-read, data/padding/length
corruption and torn handoff recovery. It also verifies byte-exact retention of
the independent SF5B golden encrypted PERIODIC/EVENT frames.

All host checks are added to `run_host_tests.sh`. They model read-side record
classification only. There is **no page header codec, reclaim-intent state
machine, page rotation, FlashBackend writer, asynchronous mutation, Gateway
runtime instance, radio/counter/ACK integration, real power-cut or authenticated
custody proof**. An apparently committed record still needs actual verified
durable storage and authenticated custody policy before ACK eligibility.

This C++ candidate is **not production firmware** and does not change the
existing 73-byte SF4B format, product flash ownership, live data, firmware
binary or frozen SF5B bytes. Full host suite on this new exact branch HEAD and
independent audit are still separate gates.

Next slice: independently review the candidate page header/version,
reclaim/maintenance semantics and the multi-family object size requirements.
Only then implement a portable store with asynchronous failure and power-cut
injection, before contemplating any Gateway flash activation.
