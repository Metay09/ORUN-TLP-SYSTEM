# SF5K3M — Authenticated single ACK to durable Tracker record release

**Status: DRAFT. New M source and tests have not yet been locally compiled
or executed. No target RAK4630 build, physical flash/KAT or real Gateway signer.**
Stacked on SF5K3L (focused real SecurityStore/HMAC host PASS).

## What changed (real production-source code)

`CustodyAckReleaseOwner` owns the unique loop-only handoff:

```
raw 56-byte ACK
 -> actual SecurityStore-v3 enrolled Gateway scope
 -> CustodyAckVerifier, full SHA256 + scoped HKDF/HMAC tag
 -> actual ProductRecordStore durably cached exact object
 -> resnapshot that same identity and compare full 32-byte digest again
 -> private ProductRecordStore::requestAuthenticatedCustodyRelease
 -> ObservationStore::requestRelease(identity) one-way flash marker
 -> ProductRecordStore::poll reads resulting flash outcome
 -> independent post-commit record lookup (released, not uncertain)
 -> CustodyAckReleaseOwner::service publishes kDurablyReleased
```

No public API can inject a forged `AuthenticatedCustodyAck` token or
a naked record identity into the new release path. The facade's release
method is **private friend-only** to the single owner. A verified token
is invalid by default, and the verifier's token now binds exact
Tracker ID and credential epoch as well as Gateway ID, current floor,
grant generation and full object digest.

`submit()` may return `kStarted` only after the real flash owner
accepted the release job. **kStarted is not delivery confirmation.**
Until `ProductRecordStore::poll()` completes, the release owner returns
`kWaitingFlash`. Only `service()==kDurablyReleased` means the
underlying ObservationStore release step committed/read back the exact
marker and ProductRecordStore independently found that same logical
record durably released.

A failed marker write, flash uncertainty, wrong/HMAC-invalid/stale
Gateway, missing durable object, duplicate ACK while pending or
ambiguous exact object cannot return durable release success. Only
one release job is in flight, and it is serialized with existing
ProductRecordStore append/cache/maintenance/prepare jobs and unread
outcomes. No ACK arrival from an ISR modifies flash or custody policy.

**Cached ciphertext lifecycle:** the old active control record may
remain physically until the next safe control-page compaction. Existing
`ObservationStore::activeControlCount`, `snapshotRetainedExact` and
`prepareNextControlCopy` already exclude records whose backing data
is durably released. The released object does not count among four
live custody objects or remain eligible for retransmission. This avoids
unsafe pre-release `requestClearExactObject` which is still disabled.
No new flash partition or format version.

## Tests added for new M work

Run `bash tools/ci/run_sf5k3m_release_host.sh` on a local checkout with
OpenSSL development library (`libssl-dev`, installed previously):

- real persistent SecurityStore v3 and Gateway policy enrollment;
- real production SHA256/HKDF/HMAC verifier (host OpenSSL primitive port);
- genuine ProductRecordStore/ObservationStore fake NOR append + exact
  ciphertext control commit and readback;
- invalid/uncommitted Gateway rejection; correct current HMAC acceptance;
- no early release after `kStarted`; duplicate ACK pending refused;
- marker flash commit and durable post-lookup;
- synchronous and asynchronous fake NOR, reboot retention;
- fail-before-write release marker, no successful release signal;
- old ACK idempotence via non-retained exact cache exclusion.

Both standard host runners include this new test, but this new test is
**not yet PASS** until user runs it and shares output.

## Not completed / release blockers

- No production main-loop wiring of RadioManager ACK mailbox into this
  owner. The new HMAC/release owner has no deployed receiver invocation.
- No genuine Gateway-local durable custody / authenticated Edge durable
  acceptance and ACK-signing authority, no mobile Gateway service.
- No relay reverse ACK routing, dedupe, hop/window timing budget.
- No authenticated Gateway grant/revoke operator transport.
- No real nRF CC310 SHA/HKDF/HMAC KAT on the RAK4631 yet, nor recent
  complete host regression and PlatformIO `rak4630` target build.
- The future composition root must serialize live admin policy changes
  with an in-flight custody release: no late policy-revocation race may
  invalidate a previously assumed scope while the flash marker commits.
  Currently policy mutation is not exposed to any production caller.
- No PRODUCT_SECURE send-owner activation. Real firmware on devices is
  unchanged. No flash preparation, DFU, PR merge or data wipe.

Leave this PR in draft and do not claim product end-to-end custody
release until the Gateway and Tracker composition owners, CC310 and
physical multi-device power-cut/airtime tests have passed.
