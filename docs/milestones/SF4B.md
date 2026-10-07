# SF4B — Gateway CustodyStore persistence foundation

Status: **POST-AUDIT SOFTWARE VALIDATION PASS — independent audit on `ea1085f132e6434b77e19de8fc49ec4337839861` returned FAIL (1 BLOCKER / 2 HIGH / 3 MEDIUM / 6 LOW). Format/store v2 fixes are implemented; fresh aggregate host validation PASS and fresh normal RAK4630 production build PASS. Independent re-audit remains pending. No physical gateway partition/runtime is allocated or activated.**

Baseline: `main@6fc9669837563e4acd297c5cc519bb5e52fa8b0f` (SF4A merged via PR #79).
Branch: `feat/sf4b-gateway-custody-store-foundation`.

## 1. Purpose

SF4A closed the ownership contract:

```text
Tracker -> Gateway durable custody -> Edge durable custody -> Backend
```

SF4B implements only the portable durable **Gateway CustodyStore** foundation
needed underneath that contract. It answers one question safely:

> Can a gateway retain an exact opaque HISTORY_SECURE observation across reset
> and power-cut boundaries, and later reclaim it only after an already-
> authenticated durable Edge handoff fact?

SF4B does **not** authorize a custody ACK wire format, gateway RF runtime,
security MAC/KDF, tracker release mutation, Edge transport or a physical flash
partition.

## 2. Ownership boundary

`CustodyStore` owns only:

- exact opaque current HISTORY_SECURE observation bytes;
- durable local `HELD` vs `HANDED_OFF_TO_EDGE` lifecycle;
- append ordering;
- exact-byte duplicate detection;
- power-cut-safe page preparation/reclaim state;
- recovery diagnostics.

It deliberately does not own:

- RF receive/admission or LoRa scheduling;
- tracker/gateway authentication;
- custody ACK construction or verification;
- `K_root` or HISTORY_SECURE decryption;
- Edge/backend transport;
- physical partition selection;
- product role/profile selection.

The store takes an abstract `FlashBackend` and a runtime page count. There is no
`NrfCustodyFlash`, no `storage_config.h` custody range and no production
`CustodyStore` instance in this slice.

## 3. Candidate on-flash format v2

The first independent audit found that v1's single reclaim-intent slot and
single handoff marker could turn torn writes into permanent capacity loss, and
that a completed historical intent could become dangerous if a reused target
page header later corrupted. Because no physical custody partition/runtime has
ever existed, SF4B replaces the candidate format now rather than carrying a
migration burden into production.

Per 4-KiB page:

```text
64-byte page header
43 x 92-byte custody records
4-byte permanently-erased record/intent gap
3 x 24-byte reclaim-intent slots
```

The 64-byte header uses a commit-first classifier:

- header body + CRC is written/read-verified first;
- the 4-byte commit word is programmed last;
- a commit-erased torn header is non-authoritative regardless of damaged
  magic/version bytes;
- committed version is stored together with its bitwise inverse so a torn or
  corrupted version is not mistaken for a genuine newer format;
- the independent 4-byte ACTIVE word is programmed only when first admission
  activates a PREPARED page.

One 92-byte custody record contains:

```text
73 bytes exact opaque protected object
3 bytes zero alignment padding
4 bytes CRC-32 corruption check
4 bytes record commit word
4 bytes Edge-handoff marker slot A
4 bytes Edge-handoff marker slot B
```

The second handoff marker is a retry reserve for a torn first marker. Production
Nrf flash backends require an erased destination; SF4B's FakeFlash now enforces
the same rule, so retry never depends on rewriting a partially programmed word.

Each reclaim-intent slot contains:

```text
target page + reserved field + target generation
CRC-32
commit word
completion word
```

The active page has three independent intent slots. Torn/staged/partial slots
are consumed and skipped; the next erased slot can authorize a new attempt.
A committed intent authorizes exactly one old target generation. After erase
and successor PREPARED commit/readback, its completion word is programmed and
verified **before** maintenance reports success. Completed or partially
completed historical intents are never erase authority.

The CRCs are storage-corruption checks, not RF authentication or cryptographic
security primitives.

A custody record becomes ACK-eligible only after:

```text
object + padding + CRC program
-> readback equality
-> record commit word programmed last
-> full record re-read/classification
```

`requestCustody() == kStarted` is never durable-custody evidence.

Exact duplicate suppression still uses full length + byte equality. No custody
fingerprint algorithm/truncation is frozen by SF4B.

## 4. Edge handoff and reclaim

The caller may mark an object handed off only after it has independently
verified the SF4A-required authenticated `EDGE_DURABLE_ACCEPT` for that exact
object.

The store then rechecks:

- page generation/handle;
- exact object bytes;
- current HELD state;

before programming the 4-byte handoff marker.

A torn/partial handoff marker is conservatively recovered as **HELD**. That can
cause a safe duplicate downstream handoff; it cannot cause premature deletion.

Normal pressure never erases a page containing a HELD or committed-corrupt
record. Queue full therefore means **refuse new custody / no ACK**, never evict
already accepted custody.

## 5. Power-cut-safe page reclaim

Page erase is background-only and never occurs inside `requestCustody()`.

The v2 transaction is:

```text
prove old page contains no HELD custody
-> choose an erased reclaim-intent slot on current append page
-> intent body+CRC program/readback
-> intent commit-last + verify
-> erase old target page
-> verify whole page erased
-> write/verify successor PREPARED page header
-> program/verify intent completion word
-> only then report maintenance success
```

On reboot, only a **committed but incomplete** intent on the current
highest-generation ACTIVE page may authorize destructive recovery. Completed or
partial-completion historical intents are ignored. A valid reused target page
with a different generation is never erased by the intent.

Three intent slots prevent one torn intent body/commit from permanently pinning
the queue. If all three slots on one active page are consumed before one reclaim
can finish, maintenance returns the explicit
`kIntentSlotsExhausted` fail-safe result; it never guesses or erases HELD
custody.

A target named by a current committed intent may be partially erased/corrupt,
because that damage can be the result of the already-authorized erase. Without
that current authority, committed corruption remains a global fail-closed
condition.

## 6. Recovery / duplicate semantics

Recovery is read-only. It distinguishes:

- erased;
- staged/non-authoritative;
- committed HELD;
- committed HANDED_OFF;
- partial commit;
- committed corruption;
- prepared/activated page state;
- reclaim-intent state;
- unsupported newer format.

A committed record with invalid CRC/static structure is a global fail-closed
fault because it may represent custody for which the tracker already stopped
replaying.

A tracker reboot may re-protect one logical History observation into a different
opaque frame. Gateway cannot dedupe those logically without `K_root`; both may
exist until a trusted decrypting downstream owner converges them. This is
expected SF4A behavior.

## 7. Capacity, wear and scan budget

Format v2 provides **43 objects per 4-KiB page**. The planning script
`firmware/scripts/sf4b_custody_capacity.py` now models:

- normal admission bytes (80-byte body+CRC + 4-byte commit);
- one successful 4-byte Edge-handoff marker;
- page-header prepare/commit/activation bytes;
- reclaim intent body/commit/completion bytes;
- page erase distribution;
- opaque-distinct duplicates after tracker reboot/re-protection;
- byte-identical late retry re-admission after the original copy was already
  reclaimed;
- pinned pages, which concentrate erase load onto fewer cycling pages;
- analytical full duplicate-scan and boot-recovery read/CRC work.

For the existing 15-minute example (`4 objects/hour/device`) with no duplicate
or late-retry multiplier:

| Devices | Pages | Region | Slots | No-Edge fill time | Approx erases/cycling-page/year |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 10 | 16 | 64 KiB | 688 | 17.20 h | 509.3 |
| 30 | 16 | 64 KiB | 688 | 5.73 h | 1527.9 |
| 50 | 16 | 64 KiB | 688 | 3.44 h | 2546.5 |
| 100 | 16 | 64 KiB | 688 | 1.72 h | 5093.0 |
| 10 | 24 | 96 KiB | 1032 | 25.80 h | 339.5 |
| 30 | 24 | 96 KiB | 1032 | 8.60 h | 1018.6 |
| 50 | 24 | 96 KiB | 1032 | 5.16 h | 1697.7 |
| 100 | 24 | 96 KiB | 1032 | 2.58 h | 3395.3 |
| 10 | 32 | 128 KiB | 1376 | 34.40 h | 254.7 |
| 30 | 32 | 128 KiB | 1376 | 11.47 h | 764.0 |
| 50 | 32 | 128 KiB | 1376 | 6.88 h | 1273.3 |
| 100 | 32 | 128 KiB | 1376 | 3.44 h | 2546.5 |

These remain planning values, not a partition recommendation or flash-lifetime
claim.

### 7.1 Full-scan budget assigned by SF4A

The store intentionally has no speculative persistent/RAM fingerprint index in
SF4B. Worst-case duplicate admission therefore scans all retained slots.
Analytical read work is:

| Pages | Admission full-scan reads | Admission CRC bytes | Recovery worst-case reads | Recovery CRC bytes |
| ---: | ---: | ---: | ---: | ---: |
| 16 | ~62.8 KiB | ~51.1 KiB | ~78.9 KiB | ~51.1 KiB |
| 24 | ~94.2 KiB | ~76.6 KiB | ~130.3 KiB | ~76.6 KiB |
| 32 | ~125.6 KiB | ~102.1 KiB | ~189.7 KiB | ~102.1 KiB |

Recovery's header component is currently O(pages²) because generation
uniqueness is checked without a dynamic allocation/index. This is acceptable as
a bounded SF4B foundation, but **latency on real nRF52840 hardware is not yet
measured**. Before SF4D freezes ACK/rendezvous timing, target measurements must
decide whether the no-index implementation fits the receive window or whether a
small bounded RAM index is justified.

The GNU++11 host ABI test also caps `sizeof(CustodyStore)` at 512 bytes; the
currently compiled host object is below that bound. This is a software bound,
not a target-RAM measurement.

Fast Edge handoff reduces queue residence/capacity pressure, but if every
observation still requires a gateway durable commit before ACK it does not by
itself reduce tracker->gateway flash write/erase rate.

The nRF52840 per-word programming/endurance limits are deliberately **not**
invented here. The revised format avoids intentional same-word retry for both
handoff and reclaim-intent failure paths; exact hardware endurance/lifetime
margin remains a later physical-partition gate using the selected hardware
source.

## 8. Focused host coverage

The SF4B host test is compiled under GNU++11 to match the RAK4630 Arduino
language level and uses ASan/UBSan + strict warnings through the aggregate
runner.

Post-audit regression coverage now includes:

- format-v2 header/version-inverse/intent/dual-handoff classifiers;
- ACK-before-durable-commit ordering;
- strict erased-destination FakeFlash parity with production Nrf backends;
- exact HELD/HANDED_OFF duplicate idempotency;
- async program and async erase completion;
- record body-before-commit power cut;
- commit physically lands but immediate caller sees failure;
- torn handoff marker retry through the second erased marker word;
- torn PREPARED header body repaired without MCU reboot;
- two separately consumed torn intent slots followed by successful third-slot
  reclaim;
- intent commit physically lands but error is reported, then read-only
  reconciliation/resume without reboot;
- partial erase recovery under committed intent;
- completion-word protection against historical stale-intent reuse;
- the independent-audit B1 scenario: reused page holds new HELD custody, its
  header corrupts, old completed intent must not authorize erase;
- activation physically lands but reports failure, then RAM cache reconciles
  without reboot;
- full queue never erases HELD custody;
- duplicate-scan read error rejects admission instead of silently writing a
  second copy;
- query read errors are distinguishable from zero/none;
- begin() fails closed on an unreconciled backend mutation;
- genuine committed unsupported-newer format;
- committed record corruption fail-closed;
- UINT64 generation boundary;
- stale handle rejected after page-generation reuse;
- capacity/wear/scan arithmetic and source-ownership guards.

The production runtime/partition boundary remains unchanged: no
`NrfCustodyFlash`, no `storage_config.h` custody allocation and no production
`CustodyStore` instance.

## 9. Independent audit disposition and remaining gates

Independent audit of `ea1085f132e6434b77e19de8fc49ec4337839861` returned:

```text
VERDICT: FAIL
BLOCKER 1
HIGH 2
MEDIUM 3
LOW 6
```

The audit correctly found a stale completed-intent erase hazard, torn-header
availability failure, single-intent-slot permanent capacity loss, mismatch
between FakeFlash and production erased-destination semantics, missing
post-failure RAM reconciliation, and the missing SF4A full-scan budget.

The current branch contains the corresponding format-v2/store/test/model fixes.
Detailed disposition is recorded in
`docs/audits/SF4B_GATEWAY_CUSTODY_STORE_AUDIT_DISPOSITION.md`.

Fresh post-audit owner evidence:

```text
aggregate host suite: PASS
RAK4630 normal build: PASS
RAM:   29,536 / 248,832 = 11.9%
Flash: 285,924 / 815,104 = 35.1%
```

The normal production footprint remains unchanged, consistent with CustodyStore
still being unwired and linker-dead in production.

Remaining gates:

1. independent focused re-audit of the exact validated head;
2. fix any real residual findings and rerun affected gates;
3. merge only after the independent gate closes.

No physical hardware test is required for this portable/no-partition slice.
Physical flash qualification belongs to the later slice that selects and wires
a real gateway storage owner.
