# ORUN ConfigStore schema v4: persisted service intent

Status: **DESIGN CANDIDATE, 2026-10-10. Not implemented. No flash layout,
runtime or wire change is made by this document.** It needs an independent
review before the implementation slice starts.

Baseline: `main@0a5e078`.

Everything not restated here is unchanged from
`ORUN_CONFIG_STORE_V2_TOKENIZED_LAYOUT.md` (partition, two-page A/B, body+CRC
then commit-last, token rules, recovery classification, maintenance behavior)
and from `ORUN_CONFIG_STATE_TOKEN_CAS_DIRECTION.md`.

## 1. Why

What a device does is today inferred at every boot from whether GNSS answers:
GNSS found means TRACKER, not found means BASE, and the USB `ROLE` override is
RAM only. In the field that means a collar whose GNSS probe fails once boots
as a receiver (continuous RX, no tracking), and a relay forgets it is a relay
after any reset. The record that could remember the intent has exactly two
fields, tracking interval and battery capacity.

Schema v4 adds the requested services to that record. It is the persistence
half only; using the saved intent at boot is the following slice.

## 2. Owner decisions already taken (2026-10-10)

1. Existing records are not erased. v4 firmware reads a v2 record, treats the
   new fields as their defaults and writes v4 from its first save onward.
2. With "tracking requested" saved, a missing GNSS never turns the device
   into a receiver. (Runtime rule; recorded here because it is why the intent
   must be stored separately from capability detection.)

## 3. Record layout

`ORC1` family, `version = 4` (next value of the reserved multiple-of-four
namespace). Big-endian. Sealed record 68 bytes; page-local retire word after
it; owned page prefix 72 bytes.

```text
off  size  field
0    4     magic = 0x4F524331 ("ORC1")
4    1     version = 4
5    3     reserved = 0
8    8     storage_generation
16   2     payload_length = 40
18   2     reserved = 0

20   4     tracking_interval_seconds
24   4     battery_capacity_mah
28   1     service_mode        0 = AUTO, 1 = EXPLICIT
29   1     requested_services  bit0 tracking, bit1 relay forwarding,
                               bit2 application receive; bits 3..7 = 0
30   2     reserved = 0

32   8     state_incarnation
40   4     state_revision

44   16    reserved = 0        room for later settings, see section 7

60   4     crc32(bytes[0..59])
64   4     commit_word (0 when committed)
68   4     retire word (page-local, outside the CRC)
```

Constraints this satisfies:

- body+CRC is exactly 64 bytes, the existing ConfigPort staging size;
- every multi-byte field stays word aligned for NVMC;
- the v2 forward-compatibility discriminator holds, so firmware that only
  knows v2 classifies a committed v4 page as `UNSUPPORTED_NEWER`: bytes 5..7
  are zero, `version & 3 == 0`, and bytes 48..51 are reserved zeros in every
  valid record, never `0xFF`.

Bytes 72..4095 of a v4 page stay erased, as bytes 52..4095 do for v2.

## 4. Semantics

| Field | Rule |
| --- | --- |
| `service_mode = AUTO` | No explicit selection saved. Runtime keeps today's legacy inference. `requested_services` must be 0. |
| `service_mode = EXPLICIT` | `requested_services` is the saved intent. Any subset of the three known bits, including none. |
| unknown mode, unknown service bit, nonzero reserved byte | Record is semantically invalid: handled like an out-of-range interval today (corruption evidence, never guessed). |

The record stores *requested* services. Capability, effective state and
applied radio state stay separate facts and are not stored. Which
combinations the runtime can actually run is an admission rule of the
configuration owner, not a format rule.

Any later change of meaning (a new service bit, a field in the reserved room)
is a new schema version, so firmware that does not know it fails closed
instead of ignoring a setting.

## 5. Reading v2, writing v4

- The classifier recognizes v2 and v4 exactly; v1 handling is unchanged.
- A decoded v2 record yields the same config with `service_mode = AUTO`,
  `requested_services = 0`. Its token is used as is.
- Committed v2 and committed v4 pages take part in the **same** lineage rule:
  generation +1, same incarnation, revision +1. A v2 page at generation N
  with a v4 page at N+1 is the normal state after the first save and recovers
  `VALID` with the v4 page active.
- Every save and the blank-partition baseline write v4. Nothing is written at
  boot merely because the recovered record is v2.
- The page-tail-erased check starts at offset 52 for a v2 page and 72 for a
  v4 page.
- Maintenance, `UNCERTAIN`, `OUTCOME_UNKNOWN` and "never auto-erase unknown
  evidence" behave exactly as today.

## 6. Going back to older firmware

Firmware that knows only v2, meeting a committed v4 page, already does the
following (existing code, no change): it does not erase or overwrite the
page, reports maintenance with an `UNCERTAIN` token, runs on the default
configuration even if an older v2 page is still readable, and refuses
configuration changes. Flashing v4-aware firmware again recovers the v4
record unchanged.

## 7. Reserved room

Sixteen payload bytes are reserved and must be zero in v4. They exist so that
the next settings (battery thresholds, RF parameters, GNSS power policy) can
be added by a later schema version with the **same physical layout**: sizes
and offsets of CRC, commit and retire words do not move, so the power-cut
qualification of section 9 does not have to be repeated for each new field.
No field is defined or implemented for that room now.

## 8. Host tests required

- v4 codec golden bytes; decode rejects every single-field violation of
  section 4;
- classifier matrix for v4 mirroring the v2 cases (erased, staged, torn,
  partial commit, committed, retired, committed-corrupt);
- property: for all valid committed v4 records the v2-only discriminator
  yields `UNSUPPORTED_NEWER` (bytes 5..7, version bits, bytes 48..51);
- recovery: v2 only; v4 only; v2 then v4 lineage; v4 then v4; every existing
  v2 recovery case re-run with v4 pages; mixed pairs with broken lineage;
- upgrade: v2 baseline, first save writes v4, cold boot recovers it, no write
  happens before that first save;
- every existing ConfigStore fault-injection case (erase, body, commit,
  readback, async timeout, unreconciled mutation) on the v4 write path;
- full host suite with sanitizers and warnings as errors.

## 9. Physical validation required before merge

On a development RAK4631, with the existing config probe images adapted to
the v4 offsets:

1. blank partition, fresh v4 baseline, cold boot;
2. a device that still holds a v2 record: boot (no write), first save,
   cold boot;
3. normal save interrupted after erase, after body+CRC and after commit;
4. a save with BLE connected (SoftDevice asynchronous path);
5. previous firmware flashed over a v4 record: defaults in effect, page not
   erased; v4 firmware flashed back: record recovered.

Items 1, 3 and 4 repeat the scoped v2 qualification for the new record size.
The non-claims of `CONFIG_STORE_V2_PHYSICAL_QUALIFICATION.md` (no mid-NVMC
brown-out, no SoftDevice-async power cut) carry over unchanged.

## 10. Not in this slice

- Using the saved intent at boot, the USB command that sets it, status
  fields and the GNSS-absent rule: the next slice, on `ConfigMutationOwner`.
- BLE/LoRa writers, authentication, caller-supplied CAS precondition.
- Any change to GET_CONFIG bytes, TLP v1 or other partitions.
