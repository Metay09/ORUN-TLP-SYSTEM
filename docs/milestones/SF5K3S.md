# SF5K3S — Gateway authorized ingress seam (host-only candidate)

Based on commit `074daa9628bab789684aa8cd2a7969802270bfca` (SF5K3R).

## Scope

- Trusted `GatewayCustodyGrantProvider` MUST source a current **durably provisioned** per-tracker/per-Gateway 32-byte `K_custody_ack`, never `K_root`.
- `GatewayCustodyAuthorizedIngress` rejects unknown/malformed/uncustodied frames and mismatched tracker, gateway, epoch, floor or grant generation **before** any store request.
- The existing `GatewayLocalCustodyOwner` remains the only signer, strictly after exact-object durable flash commit/readback.
- Grant is rechecked immediately before ACK exposure. Revoked/rotated grant suppresses the ACK; no unauthenticated raw RF bytes become trusted.
- Key scratch is wiped even on denial; RX and grant mutations must be serialized on one loop owner.

## Not yet supplied / release blockers

- No actual **persistent Gateway grant** or secure enrollment/provisioning backend; mock provider exists ONLY in host tests.
- SF5K3T adds a privileged RadioManager ACK **TX seam**, but there is still no live Gateway application composition, no trusted grant provisioning, no Tracker seal/persist/replay composition, no Edge proof path, no Relay ACK route and no credential lifecycle.
- The Gateway does **not** validate Tracker AEAD; the grant authorizes admission to bounded opaque custody, not plaintext identity.
- No runtime `main.cpp` hookup or flash layout migration; **DO NOT FLASH** this candidate onto devices as a new Gateway firmware.
- An already-OBS1 Tracker product region must NEVER be remounted as Gateway OCQ2 or wiped. Real separate Gateway physical provisioning must be reviewed before activation.
- Serialized grant revocation and RF send admission remains mandatory: a returned ACK must not be transmitted after a revocation between `takeAuthorizedAck()` and RF TX. The SF5K3T seam assumes the same loop owner serializes both; do not wire concurrent grant mutation.

## Tests

`bash tools/ci/run_sf5k3s_gateway_authorized_ingress_host.sh` tests exact 84/100 byte HMAC vectors, synchronous/asynchronous fake NOR, duplicate ACK, invalid grants, denied custody flag, post-commit revocation, failed flash write.

SF5K3S complete repository host regression and RAK4630 build **PASS** on 2026-10-21 at commit `50e1c8bb7dca885c357cc66e31100e5738569c9d` (user-supplied build log; hardware not flashed).

SF5K3T candidate: use `bash tools/ci/run_sf5k3t_gateway_ack_radio_host.sh` for an ingress host runtime regression and new RadioManager source-contract checks. Run `bash firmware/tests/run_host_tests.sh` and `cd firmware && pio run -e rak4630` for complete validation. These **remain PENDING for SF5K3T** until executed on the candidate commit. No RF hardware loop or firmware flashing is claimed.
