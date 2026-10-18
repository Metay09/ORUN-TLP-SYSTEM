# SF5K3K — Tracker custody ACK receive/real Store scope boundary

**Status:** DRAFT; no host PASS yet for K, no RAK4630 build
or physical Tracker/Gateway/Relay RF result. Stacked on SF5K3J.

## What is real production code now?

- `custody_ack.{h,cpp}` implements strict 56-byte 0x02/0x05
  ACK **untrusted header** parsing (type, reserved flags, 84/100 B
  object size, nonzero tracker/Gateway IDs, accepted epoch/floor/
  generation ranges). Invalid parse leaves output unchanged.
- `custody_ack::matchesProtectedShape` binds the claimed tracker,
  epoch, custody-requested bit and exact 84/100-byte locally cached
  PRODUCT_SECURE object header. The protected object remains opaque;
  visible fields are NOT application AEAD verification.
- The real `RadioManager::handleReceivedPacket` now recognizes the
  direct 0x05 ACK **only** in Tracker role, checks the exact receiver
  device identity, preserves 56 received bytes and link metadata in
  one loop-owned bounded mailbox, and refuses malformed, wrong
  device/role and concurrent ACKs. It cannot overwrite pending ACKs.
  A pending ACK is cleared on role change; callback memory and queue
  handling stay in the same SX1262 driver owner.
- `SecurityStore::custodyAckScopeMatches` returns only a read-only
  boolean based on a **fully recovered, idle, provisioned v3**
  store, local Tracker identity, current credential epoch,
  committed custody floor, active Gateway ID, exact grant generation
  and policy result ownership. Untrusted ACK fields never update
  Gateway policy, counters or storage. There is no K_root leak.

## Why this does NOT release records yet

A structurally valid radio ACK and a matching Gateway scope are not
proof that the Gateway holds the **scoped K_custody_ack**.

The next approval gate requires an isolated credential-bearing
CC310-backed SHA256/HKDF-SHA256/HMAC-SHA256 verifier with:

1. Full SHA256 digest of each of at most four retained exact objects
   (the existing `ProductRecordStore::snapshotCustodyCandidates`).
2. Lookup by 8-byte digest prefix and exact length, fail closed on
   zero/multiple matching candidates (including prefix collisions).
3. HKDF salt=current credential ID and IKM=K_root **inside trusted
   SecurityStore-crypto friend only**; info binds exact tracker,
   epoch, Gateway, floor, generation. Never export K_root or PRK.
4. Constant-time compare of the first 16 MAC bytes against
   HMAC of the SF5B domain+40 ACK header bytes+FULL 32-byte digest;
   not an ACK-supplied key or a wire-field-only grant.
5. Cross-check actual retained record identity and unchanged policy
   before any `ObservationStore::requestRelease()` call.
6. Single approved release owner with flash readback/reboot outcome:
   neither RF callback nor RX mailbox may directly call release.

The real SF5J1 PRODUCT_SECURE transport currently has **no
production send-owner**. This K slice **does not** activate it or
manufacture Gateway durable intake/ACK signing. A real Gateway
must confirm local durable custody or authenticated exact-object
Edge durable accept first (SF5G/H/I security boundary).

### Relay

This PR intentionally **does not forward 0x05 through a relay**.
Relay reverse-path needs an independently reviewed transport with
dedupe, loop/TTL control and ACK timing/airtime policy. A relay
must NEVER manufacture custody acceptance or validate the Gateway
MAC on behalf of the Tracker.

## New focused tests

- `test_sf5k3k_custody_ack_scope.cpp`: strict wire parser,
  exact protected-frame visible shape and **actual SecurityStore**
  grant→revoke→reenroll→floor→reboot precheck. Test deliberately
  cannot authorize a release.
- `test_sf5j_radio_transport.cpp` K-flag test: actual
  RadioManager callback buffer copy, one-entry mailbox,
  wrong-identity/flags refusal, overflow and role separation.
- Full host runner links new `custody_ack.cpp` for every
  RadioManager source invocation and covers both tests.
- Focused SecurityStore runner covers the K wire/scope test.

Only new K assertions for short iteration:
```bash
cd ~/ORUN-TLP-SYSTEM
bash tools/ci/run_sf5k3k_ack_rx_host.sh
```

**Release gates:** K host compile/run, full host regression,
`pio run -d firmware -e rak4630`, real radio/CC310 qualification,
reviewed trusted ACK key owner, Gateway durable signer,
relay reverse route and exact durable release semantics.
GitHub Actions currently cannot run due account billing lock
(`The job was not started because your account is locked due to a
billing issue.`), so CI failures convey no compiler verdict.

No production caller of ACK mailbox/crypto/release is wired.
Leave stacked PR draft and do not flash/merge to main.
