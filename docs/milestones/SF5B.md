# SF5B — TLP v2 protected product wire + custody security contract

Status: **CANDIDATE DESIGN CREATED — INDEPENDENT SECURITY/PROTOCOL AUDIT REQUIRED; NO PRODUCTION RUNTIME AUTHORIZED.**

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

Routine live report does not require custody ACK per packet.

`CUSTODY_REQUESTED` explicitly requests durable Gateway custody.

Tracker supports at most four exact outstanding custody-request objects in the
initial contract; SF5C must preserve them/release state across reset without
turning the whole backlog into fresh opaque duplicates.

## 6. Capacity warning

At the current reference SF11/BW125/CR4/5 calculation:

- PERIODIC 100 B ~= 2.216 s;
- custody ACK 56 B ~= 1.397 s;
- immediate PERIODIC+ACK ~= 3.613 s.

50 devices at 3-minute effective cadence would exceed 100% raw channel time if
every report required immediate custody ACK, before retry/relay/event traffic.

This is why live traffic and durable custody acquisition are separate and why
30-50 nodes cannot be treated as a flat 3-minute SF11 domain.

These are calculations only, not measurements/regulatory claims.

## 7. Merge gate

1. independent security/protocol audit;
2. apply findings on this branch;
3. focused re-review;
4. merge documentation only if clean.

Codec/tests belong to the next implementation slice after the byte/security
contract is independently approved.
