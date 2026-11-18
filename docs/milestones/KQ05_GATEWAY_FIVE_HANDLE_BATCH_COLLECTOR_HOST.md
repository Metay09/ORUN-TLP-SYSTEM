# KQ05 — Gateway 1..5 actual OCQ2 handle batch collector (host only)

**Status:** DRAFT. PC TEST NOT YET RUN. No radio cutover.
**Parent:** PR #349 `57c6e12dfac61f5ca028760d692bbb28e659e55e` owner-confirmed **PC PASS**:
real Tracker SecurityStore V3 scoped five-record ACK verification followed
by five separately durable selective OBS1 release markers, including
revocation, second-marker failure, power cut and recovery. PR #346 earlier
confirmed actual Gateway OCQ2 durable 1..5 object readback + HMAC host PASS.

## Why

Gateway's existing `GatewayProductCustodyRuntime` routes incoming
PRODUCT_SECURE custody-requested frames through
`GatewayCustodyAuthorizedIngress` →
`GatewayLocalCustodyOwner`, which is *one-pending* and sends a **56-byte
single-record custody ACK** per intake. The type 0x0A five-object codec,
full-digest HMAC, durable Gateway signer and verified Tracker release
owner exist separately, but have not yet been connected to the live
radio. No RF economy should be claimed until production composition
actually replaces the old per-telemetry ACK path.

## New host-only bounded batch staging

`Kq05GatewayBatchCollector`:
1. Runs on the Gateway application task, never directly from the radio
   interrupt. Must share exclusive Flash ownership and grant mutation
   serialization with the composition root.
2. Validates actual incoming 84/100-byte PRODUCT_SECURE structure and
   custody-request flag, checks the active local provisioned Gateway
   grant, then asks actual `ProductCustodyStore::requestCustody()`.
3. Stores **only one pending exact frame** until it is Flash-committed;
   after durable readback keeps **only a handle** for each accepted
   item, up to five. Does not add a separate archive or pre-ACK cache.
4. Does not mix Tracker IDs / credential epochs in a single group,
   count duplicate handles twice, or invent custody when Flash fails.
5. When five handles collected, auto-requests a group flush. For a
   group of 1–4, a trusted application scheduler can explicitly flush
   at a *future configured latency deadline*. This class does not
   assume a physical clock or invent a default transmission cadence.
6. `prepare()` uses real `Kq05GatewayDurableBatchOwner::issue()`
   each time: reread all five exact Flash objects, recheck the currently
   durable authorization including scoped HMAC key, and publish a
   canonical 56/64/72/80/88-byte authenticated frame only if every
   member remains durable.
7. If RF cannot accept the ACK, **do not call
   `markTxAdmitted()`**; the same bounded handle group remains for
   retry. The trusted caller invokes `markTxAdmitted()` *only after*
   successful TX admission and MUST serialize grant and TX changes;
   local admission is not proof Tracker received it.
8. A lost downlink can be repaired by retransmission of exact
   previously durable objects. The Gateway may re-enroll handles and
   mint a fresh MAC after readback without re-writing the object.
   Cross-Gateway relays and Edge/backend handoff remain separate.

## HOST ONLY TEST

`bash tools/ci/run_kq05_gateway_batch_collector_host.sh`

Compiled against real OCQ2 ProductCustodyStore, production batch signer
and actual full-digest HMAC with existing OpenSSL host crypto.

Test includes:
- Existing durable Gateway owner regression (torn/reboot/foreign scope).
- Five serial completed Flash records → one 88-byte authenticated ACK;
  no extra Flash writes during signing.
- Explicit 1- and 2-record flushes; no ACK before commit.
- Failed RF admission simulation leaves handles for retry; explicit
  TX-admitted notification clears only staging metadata.
- Lost ACK → durable duplicate re-ACK without new Flash write.
- No mixing another Tracker into group, duplicate handle counted once,
  no ACK for unauthorized/invalid/custody-disabled frame, no 5-byte
  counting shortcuts.
- Revoke grant before ACK preparation or before a re-preparation;
  fail without modifying caller output.
- Injected simulated NOR failure makes group fail closed.

## Not wired to live product

**DO NOT deploy as a finished product.** `GatewayProductCustodyRuntime`
still uses existing single-object 0x05 ACK and is unchanged by this PR.
The next step must explicitly integrate group staging with RF intake,
a trusted bounded flush timer, LoRa 0x0A TX admission, and Tracker
0x0A RX → `Kq05TrackerBatchReleaseOwner`. It must protect latest
ACK-less LIVE data priority and avoid pinning all Gateway RX while
waiting for flush. Multi-Tracker concurrency, group fairness,
power-cycle replay policy, and air-time/current measurements are
unqualified.

No actual board programming, no real NOR erase, no 28 KiB legacy
History cutover. P0 field-release gate stays BLOCKED.
