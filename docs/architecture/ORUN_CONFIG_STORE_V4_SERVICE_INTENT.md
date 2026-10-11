# ORUN ConfigStore schema v4: persisted service intent

Status (2026-10-10): **persistence half implemented** on
`feat/config-store-v4` (host tests and ARM builds pass). Section 9 items 2 and
5 **passed on hardware** (receiver RAK4631 `09A462BD4B275BA5`,
`firmware/tests/m7/config_v4_remote_check.sh`, 10/10). Items 1, 3 and 4 are
pending and gate the merge. The runtime half (section 11: saved intent used
at boot, `APP SERVICES` writer and status, GNSS-absent rule) is implemented on
`feat/persistent-services`, stacked on the persistence half; host tests and
ARM builds pass, and the collar check passed on hardware (section 11). An independent review of this
document was recommended and has not happened; the owner chose to proceed.

Design baseline: `main@0a5e078`; implementation baseline: `main@7782a55`.

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
3. The 16 reserved bytes of section 7 are kept.
4. The power-cut part of section 9 is repeated once on a bench device for the
   new record size.

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

Result 2026-10-10, receiver RAK4631 `09A462BD4B275BA5`, serial DFU between
steps (a reboot, not a power cycle):

| Item | Observed |
| --- | --- |
| 2 | v4 boot over its v2 record (revision 6): `source=stored`, 180 s, no maintenance; `APP INTERVAL 300` APPLIED revision 7; after reboot `source=stored` 300 s |
| 5 | `main@7782a55` over the v4 record: `source=default` 180 s, `config_maintenance=yes`, `APP INTERVAL 600` → `MAINTENANCE`; v4 again: `source=stored` 300 s, no maintenance; `APP INTERVAL 180` APPLIED revision 8 |
| 1, 3, 4 | pending (bench: blank partition, power cuts, BLE connected) |
The non-claims of `CONFIG_STORE_V2_PHYSICAL_QUALIFICATION.md` (no mid-NVMC
brown-out, no SoftDevice-async power cut) carry over unchanged.

## 10. Implementation notes

- `config_format`: `kV4*` constants, `encodeV4`/`decodeV4Body`/`decodeV4`,
  `Config` gains `service_mode` and `requested_services` (default AUTO/0, so a
  decoded v2 record reads as AUTO). `kV4*` page evidence values are appended to
  the enum; `evidenceIs*()` helpers give the store one view over v2 and v4.
- The classifier reads up to 72 bytes. "Erased" now means all supplied owned
  bytes; a v4 page given fewer than 72 bytes is supported corruption. The
  torn-prefix rule and the future-schema discriminator still look at the first
  52 bytes only, so the published contract for later schemas is unchanged
  (bytes 5..7 zero, version a multiple of four, bytes 48..51 non-FF).
- Structural rules (reserved bytes, length, CRC, zero token parts) fail the
  decode; service mode/bits decode and are judged by ConfigStore, so a bad
  value is classified as semantic corruption with its evidence intact.
- A torn write that programmed only the first word (magic) is classified as
  supported corruption, exactly as for v2 today; the store then keeps the old
  config under maintenance. Every other cut point recovers the old or the new
  record (`tests/m7/test_config_store_v4.cpp` case 8 cuts at every step).
- The three config probe images and the M7P7B flash probe target the write
  schema (`kWriteCommitOffset`, schema-aware tail check) and name v4 evidence.

## 11. Runtime use (second slice)

- `service_intent.h` owns the mapping. Admitted (runnable today): `AUTO`,
  and EXPLICIT `NONE`, `T`, `R`, `TR`, `A`. Refused as
  `kUnsupportedCombination`: receive together with tracking or relay
  (`AT`, `AR`, `ATR`) -- relay forwarding takes every received POSITION
  before the receive path, and tracking on a receiver was never exercised.
- Boot: after `ConfigStore` and `GnssManager` begin, an admitted EXPLICIT
  intent sets the legacy carrier role directly (`A` -> BASE, `R` -> RELAY,
  otherwise TRACKER; `ROLE <x> source=CONFIG`) and feeds `RequestedConfig`
  (tracking -> GNSS location source). GNSS detection no longer chooses the
  role. AUTO keeps the legacy GNSS rule unchanged.
- GNSS-absent rule (owner decision 2): with tracking requested and no GNSS
  the device stays TRACKER, tracking is `BLOCKED` (capability absent) and
  `GnssManager` re-runs the bounded detection every 10 minutes
  (`setRedetectIntervalMs`); when the module answers, tracking runs without a
  reset. Interval 0 (AUTO) keeps "absent until reboot".
- Writer: `APP SERVICES AUTO|NONE|<letters>` (T tracking, R relay, A
  receive) -> `ConfigMutationOwner` kind `kSetServiceIntent` -> ConfigStore
  -> runtime apply -> `APP SET id=.. code=.. services=.. mode=.. token=..`.
  A change applies at once: EXPLICIT sets the carrier role; back to AUTO
  returns the role to GNSS inference (`RoleController::restoreAutomatic`).
- Status: `APP SERVICES?` prints requested intent, whether it is applied,
  effective tracking/relay state (e.g. `tracking=BLOCKED_GNSS_ABSENT`),
  receive and the legacy mode. An EXPLICIT record this firmware cannot run
  (written by later firmware) is reported as `applied=LEGACY_AUTO_UNSUPPORTED`
  and the legacy AUTO role runs; it is never silently rewritten.
- While services are EXPLICIT, the RAM-only `ROLE TRACKER|RELAY|BASE`
  override is refused (it would contradict the saved intent); `ROLE?` shows
  `mode=SERVICES`.
- Unchanged: the GNSS acquisition cycle still runs whenever the module is
  present, even with tracking off (GNSS power policy is a separate axis);
  TLP v1 bytes, GET_CONFIG bytes, BLE surface.
- Tests: `tests/m7/test_service_intent.cpp`, service cases in
  `test_config_mutation.cpp` and `test_m7p7h_usb_adapter.cpp`, re-detect in
  `tests/r3/test_r3.cpp`, and startup scenario `services_no_gnss` (real
  setup()/loop(): saved `T`, GNSS never answers -> TRACKER, BLOCKED, ROLE
  BASE refused, AUTO -> BASE, `AT` refused, `T` -> TRACKER, re-detect after
  10 minutes finds the module and tracking runs).
- Physical (2026-10-10, collar RAK4631 `0E8ADE7E71531AA3` with GNSS,
  `firmware/tests/m7/services_remote_check.sh`, 9/9 PASS): AUTO after the
  upgrade (legacy TRACKER); `APP SERVICES T` APPLIED revision 2 with
  `ROLE TRACKER source=CONFIG`; `ROLE BASE` refused; after a reboot
  `ROLE TRACKER mode=SERVICES` and `applied=EXPLICIT tracking=ON`; AUTO
  round trip (revision 3, `ROLE TRACKER source=AUTO`); `T` again
  (revision 4). The collar is left with services `T`. Still to do on the
  bench: boot with the GNSS module unplugged (TRACKER, BLOCKED, re-detect).

## 12. Not in this slice

- BLE/LoRa writers, authentication, caller-supplied CAS precondition.
- Services in GET_CONFIG/BLE status, profiles, Gateway, sensor/valve or
  location-source intent.
- Any change to GET_CONFIG bytes, TLP v1 or other partitions.
