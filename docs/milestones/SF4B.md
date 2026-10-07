# SF4B — Gateway CustodyStore persistence foundation

Status: **IN PROGRESS — portable format/store and focused host tests added; aggregate host regression, RAK4630 build and independent audit pending. No physical gateway partition/runtime is allocated or activated.**

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

## 3. Candidate on-flash format

SF4B format v1 is intentionally bound to the currently frozen 73-byte
HISTORY_SECURE observation object. A future wire-size change requires an
explicit storage-format decision/version change; bytes are not silently
reinterpreted.

Per 4-KiB page:

```text
64-byte page header
45 x 88-byte custody records
72 bytes unused/reserved tail
```

One 88-byte record contains:

```text
73 bytes exact opaque protected object
3 bytes zero alignment padding
4 bytes CRC-32 corruption check
4 bytes commit word
4 bytes durable Edge-handoff/retire marker
```

The CRC is a storage-corruption check, **not** an RF authenticator and not a
security primitive.

A custody record is authoritative only after:

```text
object + padding + CRC program
-> readback equality
-> commit word programmed last
-> full record re-read/classification
```

Only then may an upper layer regard the object as eligible for a future custody
ACK. `requestCustody() == kStarted` is never ACK evidence.

Exact duplicate suppression uses full length + byte equality. No custody
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

Erasing an old page creates a special failure mode: if power disappears during
erase, the target page can lose the very header/markers needed to prove it was
safe to erase.

SF4B therefore records a durable reclaim intent on the current append page,
outside the page being destroyed:

```text
prove old page contains no HELD custody
-> write target page + target generation + CRC
-> commit reclaim intent last and verify
-> erase old page
-> verify full page erased
-> write/verify new PREPARED page header
```

On reboot, a committed reclaim intent is the only evidence that permits a
partially erased/corrupt target predecessor to be re-erased. A staged/partial
intent does not authorize deletion. An intent that cannot be interpreted
safely fails closed.

This keeps erase off the tracker ACK critical path: `requestCustody()` never
performs a page erase. Background maintenance must maintain a prepared reserve.

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

## 7. Capacity and wear model

Current candidate geometry provides **45 objects per 4-KiB page**. The script
`firmware/scripts/sf4b_custody_capacity.py` reproduces the planning arithmetic
and explicitly accounts for admission body+commit bytes, Edge-handoff markers,
page-header prepare/activation bytes, reclaim-intent bytes and page erases.
It also exposes an opaque-distinct duplicate factor for tracker reboot/
re-protection churn. Byte-identical RF retries are not counted as extra writes
because CustodyStore exact-byte dedupe suppresses them before flash mutation.

For the existing 15-minute retained-position example (`4 objects/hour/device`),
the following table assumes every produced object is committed by this one
gateway and **no Edge drains the queue** while calculating fill time:

| Devices | Pages | Region | Slots | No-Edge fill time | Approx erases/page/year if this write rate is sustained |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 10 | 16 | 64 KiB | 720 | 18.0 h | 486.7 |
| 30 | 16 | 64 KiB | 720 | 6.0 h | 1460.0 |
| 50 | 16 | 64 KiB | 720 | 3.6 h | 2433.3 |
| 100 | 16 | 64 KiB | 720 | 1.8 h | 4866.7 |
| 10 | 24 | 96 KiB | 1080 | 27.0 h | 324.4 |
| 30 | 24 | 96 KiB | 1080 | 9.0 h | 973.3 |
| 50 | 24 | 96 KiB | 1080 | 5.4 h | 1622.2 |
| 100 | 24 | 96 KiB | 1080 | 2.7 h | 3244.4 |
| 10 | 32 | 128 KiB | 1440 | 36.0 h | 243.3 |
| 30 | 32 | 128 KiB | 1440 | 12.0 h | 730.0 |
| 50 | 32 | 128 KiB | 1440 | 7.2 h | 1216.7 |
| 100 | 32 | 128 KiB | 1440 | 3.6 h | 2433.3 |

These are planning values, not measured hardware endurance and not a partition
recommendation. No erase-cycle lifetime is claimed until the exact nRF52840
endurance requirement is verified from the selected hardware source and the
product-life margin is reviewed.

Fast Edge handoff reduces **queue residence/capacity pressure**, but if every
observation still requires gateway durable commit before ACK it does **not** by
itself reduce tracker->gateway flash write/erase rate. That distinction matters
for 30-50-device product sizing.

The ~100-device row remains a stress case, not a claim that one single-channel
SF11 gateway can continuously custody that traffic; SF4C/D RF capacity remains
a separate gate.

## 8. Focused host coverage

`test_sf4b_custody_store.cpp` covers at least:

- format classifiers/geometry;
- no custody before a prepared durable page exists;
- activation -> body/CRC -> commit/readback ordering;
- no success result before durable record verification;
- exact duplicate HELD and HANDED_OFF idempotency;
- reboot recovery;
- asynchronous `FlashBackend::kPending` completion;
- body-before-commit power cut;
- commit physically landed but caller observed failure;
- torn handoff marker conservatively recovered as HELD;
- full queue cannot erase HELD custody;
- committed reclaim intent survives a partial erase/power-cut model and permits
  safe recovery;
- reboot after reclaim completed and the successor page is PREPARED, but before
  that page is activated, recognizes the historical intent as complete and
  never reapplies it to the newly prepared successor;
- unsupported-newer format fail-closed;
- committed record corruption fail-closed.

`test_sf4b_source_contract.py` locks the no-partition/no-runtime activation
boundary. `test_sf4b_capacity_model.py` locks the capacity/wear arithmetic.

Focused standalone C++ host compilation of the candidate files with
`-Wall -Wextra -Werror` + ASan/UBSan passed during development. This is not yet
aggregate repository evidence.

## 9. Remaining gates before SF4B merge

1. run the complete aggregate host suite with the SF4B tests now integrated into
   `firmware/tests/run_host_tests.sh`;
2. run the normal `rak4630` production build and confirm no partition/runtime
   activation and no unexpected footprint/layout regression;
3. run independent Astra review of the exact branch head;
4. apply real findings and rerun validation;
5. merge only after the focused independent gate closes.

No physical hardware test is required for this portable/no-partition slice.
Physical flash qualification belongs to the later slice that selects and wires
a real gateway storage owner.
