# SF5B — TLP v2 protected product wire + custody security contract

Status: **FINAL INDEPENDENT FOCUSED RE-REVIEW: PASS WITH MINOR DOC FIX — 0 BLOCKER / 0 HIGH / 0 MEDIUM; all H1-H2 / M1-M3 / L1 and follow-up N1-N3 documentation findings closed; MERGE APPROVED. No production runtime is authorized.**

Baseline:
`main@cdb9db172d178acc877b9315453e7adbd7947985` (SF5A merged).

Branch:
`design/sf5b-tlp-v2-protected-product-wire`.

Primary contract:
`docs/architecture/ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md`.

Independent audit disposition:
`docs/audits/SF5B_TLP_V2_PRODUCT_SECURE_WIRE_AUDIT_DISPOSITION.md`.

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

A new custody-specific key is per tracker + gateway + key epoch +
**custody-policy floor + custody-grant generation** and is used only for
standard HMAC-SHA256 custody ACK authentication.

Tracker custody authorization is a separate durable capability table from
delegated COMMAND slots:

```text
custody_policy_floor
custody_gateway_slot[] = (gateway_device_id, custody_grant_generation)
```

Initial maximum is four custody-capable gateways per tracker, independent from
the four command-capable delegated slots. ACK floor/generation must exactly
match the durable custody state. Lower values are stale; higher values do not
self-advance policy and are rejected until an authenticated policy update is
durably committed. Production custody remains blocked until this tracker-side
state exists.

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
-> make tracker exact protected object durably cache-authoritative
-> transmit/retry that exact object byte-for-byte
-> Gateway:
     Edge online + exact durable accept verified
       -> no Gateway flash write
       -> custody/responsibility ACK
     otherwise
       -> Gateway flash durable commit/readback
       -> custody/responsibility ACK
-> tracker durable selective release
```

Connectivity alone is never enough: Edge durable accept must be authenticated and
exact-object-bound.

Normative connected-path rule:

```text
verified Edge durable accept before fallback -> NO Gateway flash write
no verified Edge durable accept              -> Gateway flash commit required
```

The no-flash branch is **not production-enabled by SF5B**. It remains gated on a
separately reviewed authenticated exact-object EDGE_DURABLE_ACCEPT contract.
Until that later gate closes, runtime must take the local durable custody path.

Once local fallback has started, a late Edge response does not cancel an in-flight
flash mutation; the mutation is reconciled to a known result first.

Every retained durable record eventually needs a durable responsibility-transfer
fact before tracker release. Pacing ACKs changes collision timing but does not
remove their long-term airtime cost. The online Edge bypass **does** remove most
Gateway flash writes when downstream service is healthy; local flash is the
outage/uncertain-downstream safety net.

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

Completed:

1. independent security/protocol audit;
2. findings applied on this branch;
3. focused re-review;
4. N1-N3 minor documentation fixes closed.

Final recommendation: **MERGE**.

Codec/tests belong to the next implementation slice after the byte/security
contract is merged.
