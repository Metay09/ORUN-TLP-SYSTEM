# KQ05: Five-object selective GATEWAY custody ACK (wire-only slice)

**Status: DRAFT / host-only parser and structural candidate encoder; not
a real Gateway ACK sender or Tracker release owner.**
Parent: PR #343 `80e264901d07ad9fbb9de1a2bd99c34bce99f364`;
the owner's Gateway identity-bound OCQ2/v4 host test reported **PASS**.
This new protocol's host test has NOT yet been executed on the owner's PC.

## Goal and domain separation

The owner asked for the simple routine:
**send retained record(s) -> Gateway durably writes exact bytes -> batch
receipt -> Tracker releases exactly the confirmed records.**

Previous one-record Gateway custody ACK is LoRa v2 type `0x05`, 56 bytes
PER object. The KQ05 Backend-durable receipt is type `0x09` and
may only be minted by the Backend after its own durable acceptance.
**Neither protocol is interchangeable.**

Proposed new provisional LoRa **Gateway-local** type `0x0A`,
version `0x02`, relay flag exactly `0x01`, N=1..5. Wire:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | `version = 0x02` |
| 1 | 1 | `type = 0x0A` (distinct from `0x05` and `0x09`) |
| 2 | 1 | `flags = 0x01` (relay allowed) |
| 3 | 1 | `count = 1..5` |
| 4 | 8 | Tracker device ID, big-endian |
| 12 | 8 | Gateway device ID, big-endian |
| 20 | 4 | credential key epoch, big-endian |
| 24 | 4 | custody grant floor, big-endian |
| 28 | 4 | custody grant generation, big-endian |
| 32 | 8 x N | first 8 bytes of SHA-256 of EACH exact retained protected frame; strict lexicographic ascending order |
| 32+8N | 16 | Truncated HMAC-SHA256 authentication tag (NOT implemented in this PR) |

Total packet lengths are exactly **56, 64, 72, 80, 88 bytes**
for N=1..5
(32-byte header + 8N + 16-byte MAC). Five legacy single-object
ACKs take 280 bytes total, versus a prospective 88-byte single
five-object ACK (about 69% less LoRa payload, BEFORE retries and radio
overhead). This is a *format estimate*, not achieved duty-cycle data.

### Mandatory cryptographic/durability construction for next slice

The proposed MAC input is:

`"ORUN-KQ05-GW-BATCH-ACK-V1" || exact(header + 8N hints) ||
  full SHA256(object_1) || ... || full SHA256(object_N)`

*sorted in the same strict reference order as the wire*. The MAC
must use the **existing per-Tracker / per-Gateway / epoch / floor /
generation scoped K_custody_ack**, obtained from durable authorized
Gateway grants, never supplied by radio or derived from untrusted
headers. The complete max input is <= 272 bytes (existing CC310
bounded primitive limit). Each truncated 8-byte prefix is an indexing
hint, not custody evidence or a sufficient MAC check.

Before Gateway signs, require **each** exact byte sequence is known
to be durably held in the genuine ProductCustodyStore, with commit
completion, readback and current grant check. No ACK is minted for an
in-flight write, duplicate prefix ambiguity, record not found, or a
recently revoked scope. On Tracker, after independent full HMAC/scope
verification, bind each full digest to *one and only one* retained
observation; release each matched identity selectively using the
existing durable release marker. A lost/replayed ACK must be
idempotent; a partial marker failure must not release other records
by inference. No range or cumulative floor acknowledges unstored
records.

A group must belong to ONE Tracker + ONE Gateway + ONE immutable
credential epoch/grant scope. No cross-Tracker packet is legal, and a
different Gateway must hold separately scoped credentials.

### IMPORTANT: existing product limits are not solved by this codec

The current Tracker `ProductRecordStore::snapshotCustodyCandidates()`
returns at most **4 exact cached objects**, while the proposed ACK can
reference 5. **The 4-object cache limitation and the single-pending
Gateway ingress MUST be redesigned and tested BEFORE the 5-object
batch can be used.** A wire codec does not add durable cache capacity.
The eventual sender must collect/deduplicate groups across real
Gateway OCQ2 durable records and preserve fresh live-position priority
without changing Backend receipts.

### What is implemented and tested here

- Pure fixed-buffer, no-heap `kq05_gateway_batch_custody_ack`
  structural decoder and candidate serializer, not a signer or verifier.
- Count 1..5, strict ascending unique 8-byte references, rejection of
  invalid local IDs/epochs/grant generations/flags/wrong type or
  truncated/excess payload. Caller output stays unchanged on failure.
- PC-only ASan/UBSan test that rejects `0x05` single-object ACK and
  `0x09` Backend receipt, and exercises all lengths and malformed
  duplicate/reordered references.
- No new production composition, RF dispatch, credential access,
  custody completion claims, or device Flash operations.

Host command:
`bash tools/ci/run_kq05_gateway_batch_ack_wire_host.sh`

No ACK should be accepted for release until **full digest matching,
a new independently verified MAC, and durable flash release** are all
proved, including offline multi-Gateway, lost ACK, crash recovery and
radio budget tests. P0 field release remains BLOCKED.
