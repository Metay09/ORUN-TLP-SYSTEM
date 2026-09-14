# System Architecture V1 — milestone handoff

Date: 2026-09-14. Authoritative main:
`aa3bbf810a37034d9a3d9066ede4bf579646adfe`.
Branch: `architecture/system-v1`. Documentation only; no merge to main.

## Deliverables and decisions

- [System architecture](../architecture/ORUN_SYSTEM_ARCHITECTURE_V1.md): current
  evidence, target layers, independent role/profile/capability/source/power
  concepts, 10 proposed ADRs, failure matrix and staged roadmap.
- [Gap analysis](../architecture/ORUN_ARCHITECTURE_GAP_ANALYSIS.md): 24 findings,
  full impact/test/dependency fields, priorities and smallest pre-M6 change set.
- [Protocol plan](../architecture/ORUN_PROTOCOL_EVOLUTION_PLAN.md): preserve v1;
  prefer future v2 for secure generic forwarding, with explicit legacy coexistence.
- `AGENTS.md`: targeted platform-vision and boundary guidance, retaining hardware,
  milestone, development, test/build and Git discipline.

P0: none confirmed for current prototype scope. Main P1 gates: overloaded role/AUTO,
GNSS/radio/value coupling, independent identity, config/command validation,
location ownership, verified new persistence/BLE layout, critical event/contact
security, send admission and outstanding physical tests. This is not deployment
or security certification.

Keep M0–M5/R1–R4, startup identity safety, portable ID logging, existing codecs,
journal and radio owner/gate. Extract small boundaries; do not rewrite working
state machines. END_NODE/RELAY forwarding plus independent gateway service avoids
another overloaded role enum. Profiles are presets; capabilities report hardware.
PHONE ownership preserves last-known data and excludes silent GNSS overwrite.

## Exact AGENTS.md changes and reasons

1. Broaden project mission while explicitly retaining livestock priority and
   owned RAK reference hardware; prevent speculative platform implementation.
2. Replace the ambiguous “main profiles” list with legacy compatibility and
   independent role/profile/capability/service/source/power guidance; distinguish
   gateway bridging from forwarding without changing today's role behavior.
3. Clarify that capabilities remain visible even when explicit service/power
   policy leaves hardware inactive. Identify role-named power/BLE rules as
   defaults/commitments and BLE as planned M7 work, avoiding premature startup.
4. Add source ownership, explicit validity/CLEAR and last-known rules to prevent
   phone disconnect/data loss and stale-as-live assumptions in later tasks.
5. Complete the existing truncated “Architecture changes must” sentence and link
   architecture references. Record preservation, TX_DONE, storage/BLE and future
   security/safety scope gates so later sessions do not infer permission to build
   the full roadmap. Existing milestone order and commit/push rules are retained.

## Verification

Inspected production role/network/GNSS/storage/radio/power boundaries, protocol
codecs/docs, M0–M5 reports, R1–R4 and integration audits, relevant host tests,
installed core linker/flash/Bluefruit ownership and primary external references.
Checked documentation links, required gap fields, scope and whitespace.
`git diff --check` and staged equivalent pass. Only this handoff, three new
architecture documents and AGENTS.md change; firmware, build configuration,
protocol definitions and storage definitions are identical to authoritative main.

No PlatformIO build or host suite rerun was needed for documentation-only changes.
Existing test reports are cited as historical evidence, not new test results.
No physical testing, upload or production deployment was performed.

## Physical evidence and next work

Owner reports successful two-device USB/upload/role/RF transfer with matching
source/sequence and RSSI/SNR, no RF-test reset loop, and GNSS detection/start/
timeout/low-power release with production restored after temporary tests.

Still pending: open-sky ORUN fix and full GNSS→store→POSITION→BASE chain;
physical POSITION relay; injected stuck bus, watchdog and flash-cut tests;
WB_IO2/3V3_S electrical behavior; current, long-range RF and mechanical tests.

Next bounded task: establish independent legacy compatibility fixtures and
physical GNSS/slot evidence, then neutral values/identity and compatibility-mapped
config boundaries. Prove new storage ownership before persistent features;
security/receipts precede networked critical contact, and SoftDevice/bond/DFU
integration precedes BLE. PHONE, new packet types, generic relay, messaging,
actuation, second hardware and backend remain separate later tasks.

Owner decisions remain: partition/DFU budget, identity namespace, v2/legacy window,
security provisioning/library, trusted contact, deployment airtime policy,
phone persistence/freshness thresholds and later actuator/message limits.

Verdict: **ARCHITECTURE V1 READY FOR REVIEW**.
