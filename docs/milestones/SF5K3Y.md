# SF5K3Y — Gateway-scoped grant snapshot recovery candidate

**Base:** SF5K3X `4fc92038419a788dbc62bf568056484609c90bd4`.

## Motivation / ownership

SF5K3X's `GatewayDurableGrantSource` interface has no live implementation.
Tracker `SecurityStore` owns its device credentials, counters and grants on
the security region at `0xE7000..0xE9000`. A Gateway MUST NOT derive its
authority from a Tracker root, nor mount a second writer on this partition.
On a separately provisioned Gateway only, the same physical 2x4 KiB
security region may be assigned to an **exclusive Gateway-owned** format.
No live main.cpp role/owner switch is allowed in this candidate.

## Added: read-only local grant snapshot

`GatewayGrantSnapshotReader` implements both `verifiedForLocalGateway()`
and `activeGrant()`. A valid snapshot is bound to the exact nonzero Gateway
identity, signed/proven by an injected, trusted
`GatewayEnrollmentProofVerifier`, flash-persisted and fully committed.

Candidate `OGK1` is distinct from `ORS1` Tracker security, `OBS1`
Tracker observations, and `OCQ2` Gateway custody. Each of two security
pages is 4096 bytes. A page contains an immutable 32-byte header, exactly
32 56-byte scoped grants, a 64-byte detached issuer authenticity proof,
CRC32 and 4-byte commit-last marker, with the remaining bytes erased.

The format has no provisioner/writer in firmware; a future issuer must sign
all canonical bytes (not just a CRC or public visible IDs). Its signature
algorithm, trusted public-key distribution, replay/rollback prevention,
writing ceremony, and renewal lifecycle require a separate security review.
Host tests use an intentionally **forgeable fake verifier**, which is only a
way to validate parsing and authorization order.

Recovery accepts an erased/erased region as `Empty`, but never as an
authorized grant. It refuses unknown/ORS1/torn/malformed/dirty-tail pages,
duplicate Tracker identity slots, wrong local Gateway, invalid signatures,
equal active generations and read failures. Two valid, distinct signed
generations select the higher, followed by a second validation of the exact
selected page. Its verified snapshot remains **immutable in RAM**, preventing
untrusted later flash bytes from silently substituting ACK keys. Dynamic
`denyForCurrentBoot()` invalidates it and wipes those keys; a future
provisioning writer must first call this and then reboot.

The reader cannot add, rotate, revoke persistently, sign or erase any grant.
It can be injected into the existing SF5K3X Gateway startup gate. It is
**not instantiated in main.cpp** or wired to a live radio. No production
signature verifier, trusted authority anchor, initial secure enrollment
channel, factory reset or security-partition service selection exists yet.

## Significant release blockers

1. Choose and audit a concrete issuer signature scheme / pinned public key,
   physical provisioning authorization and rollback-resistant revision
   checkpoint. **A valid old signature by itself does not prevent rollback**
   if an attacker removes a newer snapshot; don't claim durable revocation
   across hostile physical rewrites from this reader.
2. Add an actual power-loss-safe Gateway-only A/B signed snapshot writer,
   serialized through `FlashMutationGate::security*` ONLY after the
   product service owner has selected Gateway; no simultaneous SecurityStore
   mutation or implicit ORS1-to-OGK1 conversion.
3. 32 entries per Gateway are an **initial bounded candidate**, not proof of
   the target 30–50/~100-device fleet density. Scale admission policy and
   memory/runtime costs before product rollout.
4. Gateway `ProductCustodyStore` OCQ2 still needs independent product-region
   ownership; no magic, copy, or erase operation is shared with OGK1.
5. Current opaque Gateway admission does not authenticate Tracker AEAD,
   requiring further spoofing/storage exhaustion protections. Edge durable
   handoff, reclaim, relay routing, RF ACK rendezvous remain separate.

## Validation requested

```sh
bash tools/ci/run_sf5k3y_gateway_grant_reader_host.sh
bash firmware/tests/run_host_tests.sh
pio run -d firmware -e rak4630
```

The focused host uses fake NOR, an intentionally fake issuer proof and the
**real** SF5K3X startup guard + OCQ2 store, proving absent/foreign/corrupt
signature/census failure closes access, a signed snapshot survives restart,
and no erase/write occurs in this candidate. It does NOT qualify issuer
authentication or a production Gateway. Tests/build are pending until run.
