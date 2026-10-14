# SF5J2A — Tracker exact protected-object durability seam

Base: `main@2c63191f4b28260d325f6f277e4ff824a288fa9b`.
Branch: `feat/sf5j2a-product-exact-cache-owner`.

## Purpose

Expose the already implemented SF5C exact-object control journal through its
single production `ProductRecordStore` owner. A future SF5J2 send scheduler
will select a retained logical record and use `ProductSecureCrypto` to
seal it once. **No protected object may be transmitted until its exact
ciphertext and tag have been committed and read back from flash.**

New APIs on `ProductRecordStore`:

- `oldestRetained(record)`: read-only logical observation selector.
- `findCachedExact(identity, value)`: recover exact persisted frame bytes
  before any new encryption; existing bytes always take precedence over a
  retry timer.
- `submitExactCache(value)`: structurally validates 84/100-byte sealed
  PRODUCT_SECURE frame against local sender identity, application family,
  custody-requested bit and backing record kind. Refuses RESULT objects here;
  SF5B uses the distinct delegated family for them.
- `takeCacheOutcome(outcome)`: for `kStarted` submissions, returns durable
  success only after the existing control journal confirms commit and
  `findExactObject` reads back *identical* bytes. `kAlreadyDurable`
  can be used only after the existing bytes were read back and matched.
- When the control page is absent or needs compaction, start one bounded
  control maintenance operation and retry the exact bytes once. A full
  four-object cache remains full, not a reason to evict or re-protect.

The existing `ObservationStore` caps outstanding cached protected objects
at four, retains immutable ciphertext across reboot, and only frees an object
when its logical record is durably released. SF5J2A reuses those contracts; it
does not create a second persistent ciphertext format.

## Scope boundary

No production use of `submitExactCache` yet, no encryption call,
PRODUCT_SECURE RF TX, Gateway receiver consumption, Edge transfer, custody
ACK, or record release. Legacy POSITION/HISTORY_SECURE operations remain
unchanged until the explicit SF5F cutover gate. Only the Tracker's existing
ObservationStore owner may mutate this product region.

The accepted opaque ciphertext is not authenticated by
`decodeProductSecureFrame`; the *future internal sender* must create its own
AEAD frame from the exact retained plaintext identity, reserve a counter
through SecurityStore, and never re-protect if a cached object already exists.
This API does not accept bytes from radio/USB or constitute inbound authority.

## Host verification

The existing `test_sf5f_product_record_store.cpp` includes new SF5J2A
regressions:

- Sync and async flash exact-byte write, readback and remount/reboot.
- Existing immutable object accepted byte-for-byte without rewriting.
- A changed security counter (retry) is rejected; foreign sender and missing
  custody-requested flag are rejected.
- An unprotected second logical record remains uncached.
- Outstanding cache capacity remains bounded to four exact objects.

Commands (fish):

```fish
cd ~/ORUN-TLP-SYSTEM
git fetch origin
git switch feat/sf5j2a-product-exact-cache-owner
git pull --ff-only
bash firmware/tests/run_host_tests.sh
echo HOST_EXIT=$status
pio run -d firmware -e rak4630
echo PIO_EXIT=$status
```

Status at creation: **candidate, host and target build not yet executed**.
Do not upload this dormant slice merely to reproduce already proven legacy RF.

## Follow-on runtime sequencing (SF5J2B)

1. Recovery: check existing cached exact object and backing logical record;
   never consume a security counter for ordinary retransmission.
2. For a fresh retained PERIODIC/EVENT: seal once using the internal crypto
   owner, begin exact-object cache write, await the durable readback outcome.
3. Schedule `RadioManager::sendProductSecurePacket` only from the verified
   cached bytes. Maintain max four outstanding and bounded airtime/retry policy.
4. Never treat `TX_DONE` as custody or release. Release only on an authorized
   Gateway custody ACK after separately durable gateway/Edge proof.
5. Enable product cutover only after physical two-node on-air tests and
   legacy retirement/rollout gates are reviewed.
