# History reboot preservation and page-erase I2C physical qualification

Status: **PASS — scoped physical qualification**

Date: 2026-10-02  
Branch: `fix/history-reboot-preservation`  
Qualified code head: `8ffbcc980dea40909083c9e0bf06797b55890cae`  
Baseline: `main@182814c6dd3cf770bafe67ede0cb98b5845b04c7`

## Scope

This record physically qualifies the History page-erase/I2C coexistence fix on
`8ffbcc980dea40909083c9e0bf06797b55890cae` and records earlier branch-lineage
power-removal evidence for committed-history recovery / sequence no-reuse.
Demand-driven read-only reboot recovery itself is host-qualified; no dedicated
physical measurement of flash program/erase counts across a no-work reboot was
performed.

The branch intentionally keeps the existing TLP v1 POSITION bytes, RF behavior,
history flash allocation/record format and circular-history capacity unchanged.

The runtime changes are narrowly scoped to:

- make history sequence reservation demand-driven after reboot so recovery and
  idle polling do not mutate flash merely because the MCU restarted;
- hold an already-promoted fresh GNSS fix through short storage reservation
  backpressure, still bounded by the existing fresh-fix age limit;
- expose the HistoryStore page-erase window and temporarily quiesce the two
  loop-owned I2C clients (GNSS and accelerometer) while that erase is physically
  pending.

## Software validation

On qualified head `8ffbcc9`:

- full host suite: **PASS**;
- RAK4630 PlatformIO production build: **PASS**;
- RAM: 28,744 / 248,832 bytes (**11.6%**);
- flash: 260,200 / 815,104 bytes (**31.9%**).

The host suite includes the existing protocol/RF/storage/startup/BLE/geofence
regressions plus focused coverage for:

- reboot recovery remaining read-only until real ticket demand exists;
- repeated reboot-without-work preserving history and avoiding metadata wear;
- lazy reservation before first post-reboot append;
- fresh GNSS fix retention while storage reservation is pending;
- fresh-fix expiry still allowing GNSS low power;
- asynchronous HistoryStore erase exposure remaining asserted for the complete
  pending erase interval and clearing before header/reservation writes.

## Physical evidence

Reference hardware: RAK4630/RAK4631 production target with GNSS and RAK1904
present.

### Reboot / power-loss recovery

A prior real power removal/recovery test preserved committed history and did not
reuse sequence identities. Immediately before the cut the log showed
`sequence=27403`, `records=681`; after recovery and the next committed fix,
the sequence advanced to `27648`, `records=682`.

The jump is consistent with the existing reserved-block no-reuse contract. It is
not evidence of record loss. The exact firmware SHA used for this earlier
power-removal sample was not independently recorded; it predates the final
`8ffbcc9` page-erase/I2C fix on the same branch lineage. It is therefore used
only as scoped evidence for committed-history recovery and no-reuse, not as
physical proof of the newer no-work/read-only reboot behavior.

### Circular history page rotation

Before the final I2C fix, physical long-run testing repeatedly showed the
expected circular-history transition from 728 valid records to 625 after one
104-record page was erased, but each rotation coincided with:

`GNSS I2C recovered; acquisition resync`

That was a real hardware regression: the asynchronous SoftDevice-backed history
page erase could remain in flight across cooperative loop passes while a Wire
transaction started on the next pass.

On qualified head `8ffbcc9`, the same physical boundary was exercised again:

- `sequence=29006`: `records=728 overwritten=0`
- `sequence=29007`: `records=625 overwritten=104`
- subsequent appends continued through at least `records=639`;
- sequence progression remained continuous;
- GNSS fixes continued normally across the rotation;
- **no** `GNSS I2C recovered; acquisition resync` occurred at the erase;
- no storage append failure was observed.

This physically validates the narrow production behavior implemented by
`HistoryStore::erasePending()` and the composition-root I2C quiesce.

## Compatibility and system impact

- TLP v1 wire bytes: **unchanged**.
- RF framing/airtime behavior: **unchanged**.
- History flash region and on-flash record/page formats: **unchanged**.
- Circular capacity: **unchanged** (728 records; one page rotation removes 104
  oldest records).
- Sequence identity rule: **unchanged** — reserved but unused identities are
  never reused after reboot.
- Store-before-send: **preserved**.
- GNSS fresh-fix maximum age: **unchanged**; storage backpressure does not make
  stale data live.
- Accelerometer ownership: unchanged; polling is only deferred during the
  physically pending history erase window.
- Security/provisioning/command semantics: **unchanged**.

## Remaining reboot-churn risk

The no-work reboot failure mode is removed: recovery and idle polling no longer
consume a reservation slot. A different bounded risk remains when every reboot
actually reaches a new fix and therefore creates legitimate ticket demand.
Each such boot may reserve a new 256-ticket block; after the active page's eight
sequence slots are consumed, metadata pressure can rotate to the next ring page
even if only a few new records were added, reclaiming up to 104 older records.
Eliminating that work-producing reboot churn would require a separate
persistence-format/policy change and is outside this PR.

## Evidence boundary / not claimed

This qualification does **not** claim:

- a controlled physical power cut at an exact NVMC/SoftDevice program or erase
  instruction boundary;
- brown-out characterization across supply-voltage ramps;
- exhaustive flash-wear lifetime qualification;
- physical validation of every other flash client performing an erase while
  GNSS is active;
- new delivery semantics — TX_DONE remains transmission completion, not remote
  delivery confirmation.

Host fault-injection tests remain the evidence for torn record/metadata/page
transition recovery. The physical power-loss test above validates committed
history recovery/no-reuse at the exercised cut point only.
