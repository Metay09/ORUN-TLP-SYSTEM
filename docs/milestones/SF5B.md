# SF5B — TLP v2 protected product wire + custody security contract

Status: **INITIAL INDEPENDENT SECURITY/PROTOCOL AUDIT: FIX THEN RE-REVIEW. Required documentation fixes are being applied on the same branch; no production runtime is authorized.**

Baseline:
`main@cdb9db172d178acc877b9315453e7adbd7947985` (SF5A merged).

Branch:
`design/sf5b-tlp-v2-protected-product-wire`.

Primary contract:
`docs/architecture/ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md`.

## 1. Scope

SF5B is documentation/design only.

It defines the exact candidate bytes/security mapping for:

- PRODUCT_SECURE PERIODIC_OBSERVATION;
- PRODUCT_SECURE EVENT;
- shared v2 one-hop opaque relay wrapper;
- exact-object GATEWAY_CUSTODY_ACK;
- first delegated CONFIG_SET_DESIRED / CONFIG_STATE_READ plaintexts;
- delegated CONFIG RESULT plaintexts.

It does not:

- implement a codec;
- activate product secure RF;
- activate Gateway custody;
- allocate physical flash;
- change TLP v1;
- change frozen HISTORY_SECURE bytes;
- claim offline phone decrypt is implemented.

## 2. Candidate sizes

```text
PRODUCT_SECURE header                 28 B
PERIODIC plaintext                    64 B
PERIODIC total                       100 B
EVENT plaintext                       48 B
EVENT total                           84 B
GATEWAY_CUSTODY_ACK                   56 B
V2 relay wrapper overhead             16 B
delegated COMMAND/RESULT max          96 B (existing frozen envelope)
```

Current SF4B Gateway CustodyStore is bound to the old 73-byte HISTORY_SECURE
object and is not reused silently.

## 3. Security direction

PRODUCT_SECURE profile 0x01 reuses the reviewed root D2A HKDF/AES-CCM
construction and the device SecurityStore TX counter.

A new custody-specific key is per tracker + gateway + key epoch + policy floor +
grant generation and is used only for standard HMAC-SHA256 custody ACK
authentication.

Gateway never receives K_root.

No group/fleet custody key is introduced.

## 4. Offline-read boundary

The first PRODUCT_SECURE profile is not a phone read key.

The authenticated security-profile byte is the compatibility seam for a later
reviewed read-capable profile. A phone is never given profile-0x01 D2A traffic
key merely to decrypt data.

A future profile must permit read access without tracker-origin forgery
authority or explicitly fail review.

## 5. Sender policy

For the initial SF5 durable tracker profile, routine PERIODIC/EVENT records are
not double-published as one best-effort live frame plus a second custody frame.

Normal policy:

```text
durable record
-> protect once with CUSTODY_REQUESTED=1
-> make exact protected object durably cache-authoritative
-> transmit/retry that exact object byte-for-byte
-> authenticated durable custody ACK
-> durable selective release
```

Every retained durable record eventually needs a durable responsibility-transfer
fact before tracker release. Pacing ACKs changes collision timing but does not
remove their long-term airtime cost.

`CUSTODY_REQUESTED=0` is only a bounded best-effort exception. A critical EVENT
may use one extra live notification when custody admission is temporarily
unavailable, but that transmission does not release the durable record and later
custody is still required.

Tracker supports at most four exact outstanding custody-request objects total
across PRODUCT_SECURE PERIODIC/EVENT and custody-eligible DELEGATED_D2GW RESULT.

A custody-requested exact object must be durably retained **before first RF
transmission**. Retry/backoff/attempt timeout never justifies re-protection.
Re-protection is limited to exact-cache loss/corruption, credential-lifetime
change, or another separately reviewed security-invalidating condition; every
replacement uses a fresh security counter and bounded diagnostics.

SF5C must preserve those exact objects plus selective release state across reset
without turning the whole backlog into fresh opaque duplicates.

## 6. Capacity warning

At the current reference SF11/BW125/CR4/5 calculation:

- PERIODIC 100 B ~= 2.216 s;
- custody ACK 56 B ~= 1.397 s;
- one custody-requested PERIODIC + ACK ~= 3.613 s;
- separate live PERIODIC + later custody PERIODIC + ACK ~= 5.829 s.

The normal initial policy is the **single custody-requested frame**, not
live+custody double publication.

Raw occupancy examples:

| effective cadence | policy | 30 nodes | 50 nodes |
| --- | --- | ---: | ---: |
| 3 min | one custody-requested frame + ACK | 60.2% | 100.4% |
| 3 min | separate live + custody + ACK | 97.1% | 161.9% |
| 15 min | one custody-requested frame + ACK | 12.0% | 20.1% |
| 15 min | separate live + custody + ACK | 19.4% | 32.4% |

These are raw occupancy calculations only; they exclude collision/retry/relay/
EVENT/COMMAND traffic. Pure-ALOHA-like contention means practical capacity is
materially lower than raw channel percentage.

Therefore 30-50 nodes cannot be treated as a flat 3-minute SF11 domain. Pacing
custody attempts is still useful for collision control, but does not erase the
per-record release cost. A later aggregate/selective batch-release design may
reduce ACK overhead only through a separately reviewed protocol change.

These are calculations only, not RF measurements or regulatory claims.

## 7. Merge gate

1. independent security/protocol audit;
2. apply findings on this branch;
3. focused re-review;
4. merge documentation only if clean.

Codec/tests belong to the next implementation slice after the byte/security
contract is independently approved.
