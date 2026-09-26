# Re-audit prompt — ORUN TLP v2 delegated command security contract V2

Repository: `Metay09/ORUN-TLP-SYSTEM`

Audit branch: `design/tlp-v2-command-security-contract`

Expected branch HEAD before audit:
`2b6bcf876dcfee2d4e24fb07af6216a0d18e6b0f`

Baseline main:
`e2a370510c595c5f4b88e94a1212fb95d848a273`

Primary target:
`docs/architecture/ORUN_TLP_V2_COMMAND_SECURITY_CONTRACT_DRAFT_V2.md`

Previous audit:
`docs/audits/TLP_V2_COMMAND_SECURITY_CONTRACT_INDEPENDENT_AUDIT.md`

Product/system architecture:
`docs/architecture/ORUN_PRODUCT_SYSTEM_ARCHITECTURE.md`

This is a **focused independent re-audit**. Do not implement firmware. Do not
change protocol/runtime code. Do not weaken TLP v1 fixtures. Do not commit or
push unless explicitly requested.

## Read first

1. `AGENTS.md`
2. `docs/architecture/README.md`
3. `docs/architecture/ORUN_CURRENT_ARCHITECTURE_RULES.md`
4. `docs/architecture/ORUN_PRODUCT_SYSTEM_ARCHITECTURE.md`
5. `docs/architecture/ADR_M7P6_SECURITY_ARCHITECTURE.md`
6. `docs/milestones/M7P6D.md`
7. `docs/milestones/M7P6F.md`
8. `docs/architecture/ORUN_GATEWAY_COMMAND_AUTHORITY_DIRECTION.md`
9. previous audit
10. V2 draft

Verify the exact branch HEAD before reviewing.

## Main objective

Determine whether V2 actually closes the old implementation-blocking findings
without introducing a new cryptographic or lifecycle flaw.

Pay special attention to:

- quota semantics;
- policy floor + per-gateway grant generation;
- active revocation propagation;
- opcode -> scope enforcement;
- OfflineUserGrant PoP/target/generation rules;
- gateway sender counter ownership;
- one-outstanding-frame rule;
- tracker replay-state lifetime;
- RESULT correlation;
- relay RAM-only custody;
- ConfigStore/CAS boundary;
- exact secure-frame/AAD layout;
- full command/RESULT airtime rather than single-frame airtime.

## Critical new design to attack: frame_key_salt

V2 adds a fresh 96-bit `frame_key_salt` and derives a per-frame AEAD key from
the delegated grant key.

Audit this very carefully.

Answer explicitly:

1. Does it actually prevent same-key/same-nonce reuse after a stale gateway
   authority-store rollback, assuming the gateway has a sound CSPRNG?
2. Is the HKDF extract/expand usage domain-separated correctly?
3. Can an attacker-controlled visible salt cause key-selection confusion,
   related-key problems, CPU DoS, or replay acceptance?
4. Must the salt itself be included in AAD? V2 currently does so.
5. What happens if the CSPRNG repeats a salt?
6. Does byte-identical retransmission correctly reuse the original complete
   protected frame rather than minting a new key/counter?
7. Does gateway cloning remain only an availability/authority-compromise problem,
   or can it still create cryptographic nonce reuse?
8. Is a simpler construction safer?

If this construction is unsafe, classify it HIGH/BLOCKER and provide the
smallest standards-based correction. Do not invent custom crypto.

## Replay / generation crash-order checks

Walk these exact cases:

- new gateway added while old gateways remain offline;
- one gateway re-enrolled after factory reset;
- one gateway revoked/compromised;
- floor advance frame delayed through relay;
- floor commit power loss before/after activation;
- higher per-gateway generation accepted, then reboot;
- old generation replay after reboot;
- replay slot reclamation;
- 5th gateway with four occupied slots;
- tracker security store corruption;
- service/reset attempt that tries to erase only delegated replay state.

No accepted old command may become valid again.

## Quota / retry checks

Check that reserve block = 1 is coherent.

Clarify:

- one new protected attempt = one counter/quota unit;
- byte-identical RF retransmission = no new counter;
- same logical command rebuilt after terminal failure = same command_id,
  new counter;
- quota refresh changes only that gateway grant generation, not every gateway;
- malicious counter jumps cannot cause another gateway's authority loss.

## OfflineUserGrant

Check that the V2 direction is coherent:

- backend-signed grant;
- app public-key identity bound into grant;
- fresh gateway challenge + proof of possession;
- tenant/site/target-set binding;
- highest authorization generation persisted;
- online revocation/min-generation check when Internet exists;
- bounded offline budget when Internet is absent;
- tracker never stores human identity/ACL.

Signature algorithm and Android/backend implementation may remain deferred, but
state whether the model is sufficient to proceed to a separate implementation
slice.

## Command freshness / CAS

The V2 draft no longer treats current ConfigStore `generation_` as automatically
safe for remote CAS.

Verify this is the right boundary.

Determine the minimum future config-state-token properties needed to avoid ABA
after corruption/fallback/reset/migration.

Do not require a generic persistent command journal for desired-state config if
CAS + resulting durable state is sufficient.

## Relay

V2 removes unauthenticated persistent custody from the first slice.

Check:

- RAM-only queue bounds;
- tag/full-frame-aware dedupe;
- one selected custodian;
- retry/retention bounds;
- reboot loss is honestly treated as availability loss;
- fake RF traffic cannot cause flash wear.

Persistent custody remains a later requirement. Confirm this can be deferred
without changing the secure inner frame.

## Product completeness / portability

Do not review this command design in isolation from ORUN's product architecture.

Confirm it does not block later:

- tracking/location/history;
- telemetry;
- sensors/activity;
- geofence/LOST;
- EVENT/alarm;
- MESSAGE;
- Command/Result;
- health/diagnostics;
- map/app/backend;
- BLE/local offline operation.

RAK4631/SX1262 is the current physical reference. Future other MCU/radio/GNSS/
sensor hardware is a portability constraint, not a reason to add a generic HAL
now.

Public protocol/persistent semantics must remain hardware-neutral.

## Scale

Do **not** treat 1000 trackers as the current target.

Use:

- ~10 baseline;
- ~30-50 medium load;
- ~100 devices in one RF domain as upper engineering stress.

If larger scale is discussed, assume explicit RF/domain/gateway partitioning.

Recalculate airtime for the V2 candidate's new 96-byte secure frame and 112-byte
relay wrapper, then evaluate the full command+RESULT chain at the above scales.

## Required output

Return:

### A. Verdict
PASS / PASS WITH FIXES / FAIL-REDESIGN REQUIRED.

### B. Old finding disposition
For F1-F25 from the old audit:
CLOSED / PARTIALLY CLOSED / OPEN / SUPERSEDED, with a short reason.

### C. New findings
For each:
ID, severity, evidence, failure scenario, required correction,
implementation-blocking YES/NO.

### D. frame_key_salt cryptographic analysis
Explicit yes/no conclusion on nonce/key safety.

### E. Replay / revocation / crash-order analysis

### F. Offline user auth analysis

### G. Command freshness / CAS analysis

### H. Wire/AAD review
Review exact 56-byte header, 96-byte max inner frame and 112-byte wrapper.

### I. Relay custody/DoS analysis

### J. Airtime/resource analysis
Use ~10 / ~30-50 / ~100, not a flat 1000-device target.

### K. Minimum corrections before owner approval

### L. Deferred work that should not block this design

### M. Final recommendation
State whether V2 can proceed to owner approval/wire freeze, or must remain draft.

## Evidence discipline

Documentation != host PASS.
Host PASS != hardware PASS.
Build PASS != RF/power/flash field PASS.
Calculated airtime != measured RF airtime.

If unknown, say UNKNOWN.
