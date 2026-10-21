# SF5K3X — fail-closed Gateway OCQ2 startup ownership gate

Baseline: `87a95902c6e5a2a625d11a8ba5e60eb20bc711db` (SF5K3W).

## What this candidate implements

`GatewayProductStartupGuard` is a one-attempt boot-only admission layer
around the EXISTING `ProductCustodyStore::begin()`. It requires:

- Explicit trusted Gateway-service authorization (**not** legacy BASE role).
- A nonzero trusted local device ID equal to the Gateway's provisioned ID.
- A separate proof that the Tracker's `ObservationStore` is NOT mounted
  on the product partition. Only the composition root may supply this fact.
- A `GatewayDurableGrantSource` which verifies *locally persisted, recovered
  and authenticated* enrollment. No current production implementation exists.
  Test fake enrollment never grants real custody permission.
- A working physical product flash backend, no unreconciled mutation, a
  read-only full-region census that refuses OBS1 and OCQ1 pages, and the
  OCQ2 store's own strict recovery of erased or valid OCQ2 records.
  Unknown, corrupt, torn or mixed pages never trigger an automatic erase.

The gate is one-shot per boot: a failed mount cannot be retried in the same
running firmware after an untrusted role switch. `mayServe()` must be checked
on EVERY runtime pass and immediately becomes false on service deactivation,
identity change, tracker ownership, durable grant revocation, store fault or
unreconciled physical mutation.

`GatewayProductCustodyRuntime::updateGuarded(gate, service, tracker_mounted)`
makes the authorized boot gate usable with the SF5K3W existing live
RX -> flash -> ACK composition; the older `update(bool)` remains its test/
low-level seam. Neither path is invoked from `main.cpp` yet.

## Safety / unimplemented boundaries

The production `main.cpp` STILL mounts `ProductRecordStore` as a possible
Tracker writer, regardless of legacy role. It does NOT instantiate or call
the Gateway boot guard; that is intentional until persisted Gateway service
selection and the product-region single-owner handoff are reviewed. No
hard-coded or synthetic `GatewayDurableGrantSource` will be added.
Gateway grant enrollment, key derivation/provisioning, rotation/revocation
and secure persistence remain a **separate incomplete feature**. The
Tracker-side SecurityStore v3 policy journal is NOT proof that the Gateway has
its own 32-byte scoped ACK key. Never import a Tracker K_root into Gateway
simply to make the test pass.

The Gateway currently accepts authorized opaque frame shapes; it does not
prove source AES-CCM authentication and thus still needs intake/rate/storage-
exhaustion protections. No Edge handoff, GC/eviction, half-duplex ACK timing,
relay route or physical LoRa test is claimed. An already-OBS1 Tracker product
region is NOT eligible for automatic Gateway conversion. If OCQ2 becomes
full, fail closed rather than erasing held custody.

## Host checks and validation status

`bash tools/ci/run_sf5k3x_gateway_startup_guard_host.sh` uses the actual
`ProductCustodyStore` and `product_region` census on fake NOR. It checks
disabled service, incorrect device ID, tracker ownership, unprovisioned
enrollment, failed reads, unreconciled operation, OBS1/OCQ1, unknown/torn
OCQ2, erased/read-only mount, revocation and reboot after a committed
custody object. It does NOT prove a durable grant store or real radio.

Required before merge/activation:
```sh
bash tools/ci/run_sf5k3x_gateway_startup_guard_host.sh
bash firmware/tests/run_host_tests.sh
pio run -d firmware -e rak4630
```
These tests have NOT been executed on the new commit until the operator
reports the results. No device upload/flash wipe permitted.
