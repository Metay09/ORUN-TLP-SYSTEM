# TLP v2 Delegated Command Security — Audit Disposition

Status: **CONSOLIDATED AUDIT DISPOSITION — OWNER-APPROVED DESIGN DIRECTION; DOCUMENTATION-ONLY.**

Baseline main:
`e2a370510c595c5f4b88e94a1212fb95d848a273`

Current design:
`docs/architecture/ORUN_TLP_V2_DELEGATED_COMMAND_SECURITY_DIRECTION.md`

This file is the retained audit record for the delegated-command design. Raw
review prompts and intermediate reviewer transcripts were intentionally removed
after their actionable findings were incorporated, to keep `docs/audits/`
focused on durable engineering evidence rather than conversation artifacts.

No firmware, test fixture, protocol runtime, RF setting or hardware behavior was
changed by this audit/design sequence.

## 1. Review chronology

### First independent audit

Target:
`3af79af8a52de779cedcfc05fc76c41e91fea5b5`

Verdict:
**FAIL / REDESIGN REQUIRED**

The first draft's overall offline store-forward goal was considered salvageable,
but the review found implementation-blocking problems around:

- quota/reserve-ahead semantics;
- global gateway policy epoch behavior;
- revocation propagation;
- opcode/scope binding;
- offline user authorization;
- gateway rollback and nonce reuse;
- delegated replay-state lifecycle;
- unauthenticated persistent relay custody;
- dedupe/serialization/custodian behavior;
- ConfigStore CAS assumptions;
- RESULT correlation;
- full-chain airtime accounting.

The first draft remains superseded.

### V2 independent re-audit

Reviewed V2 content:
`44ead40468e5f9a3374b9656fe48d77b06339212`
(content equivalent to the V2 design immediately below its prompt-only commit).

Verdict:
**PASS WITH FIXES**

No BLOCKER or HIGH finding remained.

The re-audit accepted the redesigned core:

- per-tracker delegated grant derivation without exposing `K_root`;
- tracker-wide policy floor + per-gateway grant generation;
- shared grant quota/counter/HWM semantics;
- fixed opcode -> scope authorization;
- OfflineUserGrant proof-of-possession direction;
- delegated replay state tied to the credential security lifetime;
- RAM-only bounded first relay custody;
- strict one-outstanding-frame sender rule;
- RESULT correlation requirements;
- delegated-only secure frame direction;
- ~10 / ~30-50 / ~100 engineering-scale framing.

It requested focused corrections around HKDF simplification, delegated-only
header scope, timeout/custody semantics, atomic replay-slot persistence, CSPRNG
requirements, single-custodian selection, monotonic floor synchronization and
documentation reconciliation.

### Final focused verification

Verified post-re-audit content at:
`10bc3965fd9dd718d7571aac61cdf7c298e465f1`
with the branch prompt-only head at
`ea0ed42218d87e51c4edfc643c0ee3f832372922`.

Verdict:
**PASS WITH MINOR DOC FIX**

No new BLOCKER/HIGH finding.

All nine requested focused corrections were accepted as closed. Four small
documentation corrections remained and were then applied:

- **D1:** gateway reboot cannot shorten the uncertainty window for a frame that
  may still exist in relay custody; timeout remains `UNCONFIRMED` and includes
  a scheduling/clock safety margin;
- **D2:** revoked-gateway residual exposure includes every issued grant
  generation not yet made stale at the tracker, while quota remains shared
  across scopes and is therefore not multiplied by scope count;
- **D3:** old generic `SECURE_APP` naming was corrected to
  `DELEGATED_SECURE_APP`;
- **D4:** the first relay wrapper explicitly allow-lists only delegated secure
  type 0x01; later compact secure types require an explicit reviewed allow-list
  change.

D1-D4 were applied in design commit:
`7b9888be84256d019d4eaa010cd82b519ee5bf1c`.

## 2. Final accepted design direction

The reviewed delegated-command direction now consists of:

- backend-owned gateway enrollment and user authorization;
- no tracker-side human/phone ACL;
- no gateway access to tracker `K_root`;
- per-tracker/per-gateway delegated grant material;
- two-level revocation lifecycle:
  `gateway_policy_floor` + `gateway_grant_generation`;
- monotonic/idempotent floor advancement;
- one grant-wide quota/counter/HWM across its scopes;
- reserve-ahead = 1 for the first sparse command-plane design;
- per-frame 96-bit random diversification value;
- standards-based HKDF hierarchy;
- hardware-TRNG or TRNG-reseeded DRBG requirement, fail-closed on RNG failure;
- 13-byte AES-CCM nonce retaining key epoch + direction + durable counter;
- fixed opcode -> required scope enforcement;
- maximum four active delegated command-capable gateway slots per tracker floor;
- atomic commit-last
  `(gateway_id, grant_generation, replay_bound)` persistence invariant;
- delegated replay/floor state reset only with a new credential lifetime;
- OfflineUserGrant with app-key proof of possession, target/site binding,
  monotonic authorization generation and bounded offline budget;
- transport timeout represented as `UNCONFIRMED`, not false `FAILED`;
- first relay custody bounded and RAM-only;
- first topology uses one explicitly configured command custodian per RF/site
  domain, independent of legacy Role;
- only authenticated target RESULT may produce user-visible `APPLIED`.

## 3. Delegated secure-frame status

The 56-byte header / 96-byte maximum inner frame is a
**delegated-command-specific design candidate**, not a universal TLP v2 header.

Routine tracking, telemetry, EVENT and other normal D2A/A2D traffic must not be
forced to pay this delegated-command overhead. Their compact secure types remain
a separate later design.

The first relay wrapper explicitly accepts only
`DELEGATED_SECURE_APP` type 0x01.

The delegated authority/header direction is **owner-approved as design direction**.

It is **not a wire freeze**.

## 4. Items intentionally still provisional

The following are not closed by this audit and must remain separate slices:

- COMMAND/RESULT plaintext bytes;
- ConfigStore state-token/CAS width and ABA-safe semantics;
- exact OfflineUserGrant signature suite and mobile/backend implementation;
- SecurityStore v3 exact on-flash record layout;
- gateway authority-store partition/platform;
- backend/HSM credential derivation custody;
- persistent relay custody/admission authentication;
- compact secure A2D/D2A position/telemetry/event layouts;
- OPEN_BLE, RF configuration, actuation, DFU and MESSAGE freshness semantics;
- physical sleepy-tracker relay delivery;
- measured RF/power behavior.

## 5. Evidence boundary

The independent reviews were documentation/source reviews only.

They do **not** establish:

- host-test PASS;
- production build PASS;
- physical RAK PASS;
- CSPRNG quality;
- APPROTECT/device extraction resistance;
- gateway storage rollback resistance;
- backend/HSM custody;
- measured RF airtime/range/power.

Airtime values in the design are calculations, not physical measurements.

## 6. Merge/implementation meaning

Audit disposition:

**No known BLOCKER/HIGH remains in the delegated authority/header design after
the recorded corrections.**

The owner has approved the **architecture/design direction only**.

This does not authorize production secure-RF implementation and does not freeze
COMMAND/RESULT application bytes.

## 7. Post-disposition CAS follow-up

After this delegated-command audit disposition, the separate documentation-only
`docs/architecture/ORUN_CONFIG_STATE_TOKEN_CAS_DIRECTION.md` slice resolved the
config application precondition direction at an opaque 96-bit state token with
tracker-final stale checking.

That later CAS contract was **not** part of the audit targets recorded above.
Nothing in this disposition should be read as an independent PASS for the CAS
token width, recovery/ABA semantics or tokenized ConfigStore migration. Those
items require their own focused review before implementation/wire freeze.
