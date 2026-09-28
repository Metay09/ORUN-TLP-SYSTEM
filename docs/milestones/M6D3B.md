# M6D3B — Durable GeofenceStore + physical flash owner

Status: **SOFTWARE + INDEPENDENT AUDIT PASS; READ-ONLY PHYSICAL PREFLIGHT PASS; DESTRUCTIVE A/B / REBOOT / POWER-CUT / MERGE PENDING**.

Baseline: `main@e566125afb9a08aaa5da8656d22d82e6a7580aed` (M6D3A merged via PR #54).
Branch: `feat/m6d3b-geofence-store`.
Current audited code-equivalent head: `bf1a753703c9a51381a0e0a31f631dba95a46c3d`.

## 1. Purpose

M6D3A froze the persistent byte format and reserved two flash pages. M6D3B adds
the actual durable owner and the physical-flash concurrency path, while still
leaving M6D2 production geofence behavior unconfigured.

Product invariant:

```text
one complete committed area set
-> survives reboot
-> stale/torn/corrupt evidence never becomes CLEAR
-> token authority is VALID only when recovery proves one coherent lineage
-> failed/timeout mutation is never reported as "definitely did not commit"
-> no flash client bypasses the existing single physical arbiter
```

M6D3B does **not** connect the recovered snapshot to
`GeofenceConfirmationCoordinator`, does not add BLE/LoRa/USB geofence
mutation, and does not change TLP/RF/GNSS behavior.

## 2. Durable resource owner

New `GeofenceStore` owns only:

- one complete canonical M6D3A snapshot;
- physical generation;
- independent 96-bit geofence state token;
- CLEAR / CONFIGURED / UNAVAILABLE recovery state;
- A/B persistence/reconciliation.

It deliberately does not depend on or own:

- GNSS;
- PositionFlow;
- M6D2 confirmation;
- runtime cadence;
- RF;
- BLE;
- backend/UI.

Resource state and token authority remain separate:

```text
GeofenceResourceState:
  UNAVAILABLE
  CLEAR
  CONFIGURED

GeofenceTokenState:
  UNAVAILABLE
  VALID
  UNCERTAIN
```

A readable semantic fallback may therefore be CLEAR/CONFIGURED while CAS token
authority is UNCERTAIN. In that state mutations remain blocked.

## 3. Blank-partition baseline

All-erased `0x0E5000..0x0E7000` is **not** interpreted directly as CLEAR.

With a production incarnation source available and SoftDevice still disabled,
`begin()` establishes:

```text
page A:
  generation = 1
  fresh non-zero CSPRNG incarnation
  revision = 1
  authoritative CLEAR
page B:
  erased
```

The baseline uses the same persistence sequence as later writes:

```text
body + CRC
-> readback
-> commit word last
-> full readback
-> publish authority
```

If entropy is unavailable, a blank partition remains UNAVAILABLE and no flash
write is attempted.

## 4. Recovery matrix

### Both pages erased

- resource: UNAVAILABLE before baseline;
- token: UNAVAILABLE;
- no implicit CLEAR;
- fresh baseline may be established only through the explicit CSPRNG path.

### One valid committed + other erased / definitely uncommitted-torn

- committed page remains authoritative;
- token remains VALID.

### One valid committed + exact staged successor

A staged record has a verified body+CRC and an **erased commit word**.

If it is the exact generation/revision successor in the same incarnation:

- staged candidate is never published;
- previous committed snapshot remains authoritative;
- previous token remains VALID because commit-erased is definitive evidence
  that the candidate did not activate.

### One valid committed + partial commit / committed corruption / dirty reserved tail

- preserve the committed semantic snapshot when safe;
- token becomes UNCERTAIN;
- maintenance/reconciliation required;
- mutation blocked.

### Two valid committed pages, same incarnation, exact successor lineage

- higher generation/revision is authoritative;
- token VALID.

### Two committed pages with different incarnations

- contradictory authority;
- resource UNAVAILABLE;
- token UNCERTAIN;
- never select by generation alone.

### Two committed pages with exact lineage plus reserved-tail corruption

Exact lineage still proves which semantic snapshot is newer:

- preserve the newer snapshot as read-only fallback;
- token UNCERTAIN;
- maintenance required.

Without exact lineage, different semantics fail closed to UNAVAILABLE. If
semantics are identical, the snapshot may be preserved as read-only fallback
with UNCERTAIN token.

### Unsupported/newer schema

- never auto-erase;
- resource UNAVAILABLE;
- token UNCERTAIN;
- mutation blocked.

## 5. Normal whole-resource mutation

REPLACE and CLEAR both use one A/B transaction:

```text
require VALID authority
-> canonicalize complete desired snapshot
-> if identical under VALID authority: no-op
-> inactive-page erase
-> body+CRC program
-> body readback
-> commit word program last
-> full record readback + classifier verification
-> publish snapshot + generation + token
```

Successful semantic change advances:

```text
generation += 1
revision   += 1
incarnation unchanged
```

A failed operation returns only **not confirmed successful**. It never means the
candidate definitely did not commit.

If the backend reports an accepted-but-unreconciled timeout, GeofenceStore waits
for FlashMutationGate's late definitive event before read-only two-page
reconciliation.

## 6. Physical flash owner

New sibling backend:

`NrfGeofenceFlash`

owns only:

`0x0E5000..0x0E7000`.

It preserves the existing NrfHistory/Config/Security discipline:

- exact region bounds;
- 4-byte alignment;
- program only erased destination;
- no page crossing;
- SoftDevice-disabled synchronous primitive only;
- physical readback after program/erase;
- exact maximum single program size = **560 bytes** (M6D3A body+CRC).

It is not a generic storage HAL.

## 7. FlashMutationGate integration

Geofence becomes a fourth ORUN client of the existing single physical arbiter.

Frozen normal admission order:

```text
SEC_CRITICAL
> History
> Config
> Geofence
> SEC_MAINT
```

Rationale:

- security-critical reservation/provisioning work blocks protected operation;
- live History/store-before-send must not be delayed by human-driven fence edits;
- existing small Config mutations retain their previous place;
- geofence commits are larger and human/config driven;
- routine security maintenance remains lowest normal class.

A staged request older than the existing 4000-ms operation budget enters an
aged tier above every fresh request, including fresh SEC_CRITICAL. Among aged
requests, the oldest staged request wins; normal priority is only the
deterministic tie-breaker. This closes the audit-proven starvation hole where
mapping aged work merely to SEC_CRITICAL could still lose forever to freshly
polled SEC_CRITICAL callers.

An admitted physical mutation is never preempted.

Geofence also inherits the M7P7A ownership-transfer invariant:

- accepted operation timeout -> application-level kFailed;
- shared flash token remains owned;
- slot is quarantined;
- only the definitive late Nordic SUCCESS/ERROR event releases ownership;
- GeofenceStore exposes token authority as UNCERTAIN until reconciliation.

There is still one SoftDevice event owner and one shared InternalFS/ORUN flash
token. No second `sd_evt_get()` consumer is introduced.

## 8. RAM/flash impact expectation

The production `FlashMutationGate` instance gains:

- one Geofence slot;
- one diagnostics block;
- one exact 560-byte owned asynchronous program staging buffer;
- one small synchronous `NrfGeofenceFlash` object.

No production `GeofenceStore` instance is wired in M6D3B. Therefore the
store's snapshot/blob buffers should remain linker-dead-stripped from the normal
image; only the gate-client footprint is expected to remain.

Validated production build at `59145cbe9badabdaf58b38004562ca59964e117b`:

- RAM: **24,224 / 248,832 bytes = 9.7%**
- Flash: **252,540 / 815,104 bytes = 31.0%**
- M6D3A production baseline: 23,584 B RAM / 250,476 B Flash
- M6D3B production delta: **+640 B RAM, +2,064 B Flash**

The production build succeeded with the application-ceiling and exclusive-owner
post-link guards active. This is software/build evidence only.

## 9. Host evidence added

### GeofenceStore

`test_m6d3b_geofence_store.cpp` covers:

- blank -> real CSPRNG-backed CLEAR baseline;
- blank + unavailable entropy -> UNAVAILABLE/no write;
- REPLACE, reboot recovery and CLEAR revision advance;
- unchanged VALID resource no-op;
- exact staged successor leaves previous token VALID;
- partial commit / committed corruption -> semantic fallback + UNCERTAIN;
- exact-lineage dirty-tail fallback preserves newer semantics + UNCERTAIN;
- contradictory committed incarnations -> fail closed;
- unsupported/newer evidence -> non-destructive lockout;
- dirty reserved tail -> UNCERTAIN;
- commit physically lands but backend returns failure -> result remains false,
  then recovery may prove the successor and restore VALID;
- accepted/unreconciled failure blocks recovery until physical ownership is
  reconciled;
- partial erase fault;
- boot read failure.

### FlashMutationGate

`test_m6d3b_flash_gate.cpp` covers:

- exact geofence bounds and synchronous 560-byte body program;
- Config > Geofence;
- Geofence > SEC_MAINT;
- caller buffer copied into gate-owned 560-byte staging;
- aged Geofence > fresh Config anti-starvation behavior;
- aged Geofence > fresh SEC_CRITICAL anti-starvation regression;
- accepted timeout -> quarantine -> late completion release.

All older gate tests are still linked against the new sibling backend so the
existing History/Config/Security contracts must remain green.

### Source ownership

`test_m6d3b_source_contract.py` proves:

- no production `GeofenceStore` instance;
- no production `.geofencePort()` use;
- store header does not depend on GNSS/PositionFlow/Radio/M6D2/BLE owners;
- read-only physical preflight contains no flash mutation API/primitive;
- preflight PlatformIO target does not link GeofenceStore or NrfGeofenceFlash;
- destructive qual target explicitly links both.

## 9.1. Validation evidence at 59145cbe

The complete host suite passed after fixing the source-contract test to strip
comments before checking forbidden owner symbols. The earlier PositionFlow
failure was a test false positive caused by the explanatory header comment
"No GNSS, PositionFlow..." and did not represent a C++ dependency.

Observed PASS lines include:

- `M6D3B source ownership/activation contract: PASS`
- `M6D3B GeofenceStore recovery/mutation checks: PASS`
- `M6D3B FlashMutationGate geofence client checks: PASS`
- all pre-existing B1A..B4, M3..M7, R2..R4 and production-startup scenarios.

RAK4630 builds:

- `rak4630`: SUCCESS; RAM 24,224 B; Flash 252,540 B.
- `rak4630_m6d3b_geofence_preflight`: SUCCESS; RAM 8,740 B; Flash 58,588 B.
- `rak4630_m6d3b_geofence_qual`: SUCCESS; RAM 10,664 B; Flash 68,660 B.

No image has been uploaded to hardware in M6D3B yet. Therefore:

- physical flash preflight: **NOT RUN**;
- physical A/B persistence/reboot: **NOT RUN**;
- physical power-cut qualification: **NOT RUN**.

## 9.2. Post-audit fix validation at f0d3a76d

The independent pre-physical audit returned PASS WITH FIXES and identified two
merge/destructive-qualification blockers plus one source-contract test weakness:

1. an aged Geofence request could still starve behind a continuous stream of
   fresh SEC_CRITICAL requests because aging only promoted it to the same
   numeric priority;
2. the destructive qualification image could exceed the framework's 4-KiB
   loop-task stack on the fresh-baseline path due to nested record-sized local
   buffers;
3. the forbidden-owner source-contract regex used an escaped boundary pattern
   that could fail to match real forbidden symbols.

All three were corrected on this branch.

Post-fix evidence at `f0d3a76d29c5bd5dd75691ffc2d63f4fed458361`:

- complete `./firmware/tests/run_host_tests.sh`: **PASS**;
- `M6D3B source ownership/activation contract: PASS`;
- `M6D3B GeofenceStore recovery/mutation checks: PASS`;
- `M6D3B FlashMutationGate geofence client checks: PASS`;
- production `rak4630`: **SUCCESS**, RAM **24,224 B**, Flash **252,748 B**;
- read-only preflight: **SUCCESS**, RAM **8,740 B**, Flash **58,588 B**;
- destructive qualification image: **SUCCESS**, RAM **14,588 B**, Flash
  **68,596 B**.

The qualification RAM increase is intentional: record-sized recovery,
verification and diagnostic workspaces were moved off the 4-KiB task stack into
process-lifetime/static storage. The production image still has no
`GeofenceStore` instance, so production RAM remains unchanged at 24,224 B.

No M6D3B image has yet been uploaded to hardware. Physical preflight,
persistence, reboot and electrical power-cut evidence therefore remain
**NOT RUN**.

## 9.3. Focused independent re-audit PASS

Focused Astra re-audit on exact head
`bf1a753703c9a51381a0e0a31f631dba95a46c3d` returned **PASS**.

It independently confirmed:

- aged Geofence work outranks fresh SEC_CRITICAL after the aging threshold;
- oldest staged request wins among aged work, with normal priority only as a
  deterministic tie-breaker;
- the frozen fresh-request order remains
  `SEC_CRITICAL > History > Config > Geofence > SEC_MAINT`;
- timeout/quarantine/late-completion ownership behavior is unchanged;
- measured qualification stack usage remains below the 4-KiB loop-task stack;
- production still has no GeofenceStore instance;
- source-contract forbidden-owner matching is effective;
- full host suite, production build, preflight build, qualification build and
  `git diff --check` are clean.

Durable audit disposition:
`docs/audits/M6D3B_PRE_PHYSICAL_AUDIT_DISPOSITION.md`.

The audit explicitly authorizes only the **read-only physical preflight**.
Physical persistence, reboot persistence and power-cut behavior remain unproven.

## 9.4. Physical read-only preflight PASS

On the intended development RAK4631, the audited read-only preflight image
reported both reserved geofence pages as physically blank:

- page A: `ERASED`, `all_ff=yes`, `tail_ff=yes`, CRC32 `F154670A`;
- page B: `ERASED`, `all_ff=yes`, `tail_ff=yes`, CRC32 `F154670A`;
- overall: `QUALIFICATION_IMAGE_MAY_BE_USED`.

The USB serial device reconnected and the second boot repeated the same result.
No geofence flash mutation was possible in this image.

Detailed physical record:
`docs/audits/M6D3B_PHYSICAL_QUALIFICATION.md`.

This authorizes the destructive qualification image on this exact development
unit, but does not yet prove A/B persistence, reboot persistence or power-cut
recovery.

## 10. Physical qualification safety gate

M6D3B is the first geofence slice allowed to mutate physical flash.

### Stage 1 — read-only preflight

Target:

`rak4630_m6d3b_geofence_preflight`

This image links **no GeofenceStore and no NrfGeofenceFlash** and contains no
`sd_flash_write` / `sd_flash_page_erase` path.

It only reads:

`0x0E5000..0x0E7000`

and reports, per page:

- classifier evidence;
- full-page all-FF result;
- reserved-tail FF result;
- page CRC;
- first non-FF byte when present.

Destructive qualification is forbidden unless both pages report:

`all_ff=yes`.

Any unknown/non-erased result is a stop condition and must not be automatically
cleaned.

### Stage 2 — destructive persistence qualification

Target:

`rak4630_m6d3b_geofence_qual`

PRECONDITION: Stage 1 on that exact development device reported both pages
all-FF.

It uses the real:

- GeofenceStore;
- NrfGeofenceFlash;
- NrfGeofenceIncarnationSource.

On the first blank boot it establishes the real CLEAR baseline. Operator commands
then exercise:

- STATUS;
- REPLACE with a test-only valid triangle;
- CLEAR;
- CLEAN of only `0x0E5000..0x0E7000`.

CLEAN is terminal and requires power-cycle afterward.

The test fixture is never a production fence and is never wired into M6D2.

### Electrical power-cut evidence

Host fault injection covers body/commit/erase/readback ambiguity, but does not
prove real NVMC/SoftDevice electrical timing. After normal physical A/B
qualification is clean, at least one focused real power-cut boundary probe must
be performed before M6D3B is called physically closed. That probe is a separate
operator step and must not be conflated with host PASS.

## 11. Wear boundary

One normal semantic mutation erases one inactive 4-KiB page.

Using the architecture's conservative 10,000 erase-cycle-per-page floor as
engineering arithmetic gives about 20,000 alternating A/B mutations across the
pair. This is not a hardware lifetime guarantee.

Geofence mutation must remain human/config driven; never use this store as a
tracking/telemetry log.

## 12. Explicit non-goals

M6D3B does not implement:

- M6D3C durable snapshot -> M6D2 runtime provider;
- production GeofenceStore instance;
- BLE geofence writer;
- LoRa geofence resource transfer;
- protected command/CAS transport;
- secure OUTSIDE EVENT;
- backend/mobile map UI;
- group/fleet orchestration;
- FREE_GRAZE;
- LOST semantics.

TLP v1 bytes, RF parameters, History/Config/Security formats, BLE bond pages,
R3 GNSS protections and M6D2 behavior remain unchanged.

## 13. Validation/merge gates

Required order for this storage/concurrency slice:

```text
focused host tests
-> complete host suite / warnings / ASan/UBSan
-> production RAK4630 build + RAM/Flash
-> preflight/qual probe builds
-> independent Astra audit before destructive physical test
-> fixes + affected retest
-> read-only physical preflight
-> focused destructive A/B/reboot qualification
-> focused electrical power-cut qualification
-> final audit disposition
-> merge
```

The audit-before-destructive-test ordering is intentional for this slice: the
first physical geofence mutation should not be performed until an independent
reviewer has confirmed the region ownership/recovery path.

No software result may be reported as physical persistence/power-cut PASS.
