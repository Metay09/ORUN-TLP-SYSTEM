# ORUN step 3 — real security (replacing the bench keys)

Status: **PLAN, owner-approved direction 2026-10-25.** No wire, flash or key
format is frozen by this document; each slice below carries its own design
note, host vectors, independent review and physical check, as
`AGENTS.md` and `ADR_M7P6_SECURITY_ARCHITECTURE.md` require.

## Where we are (2026-10-25)

The whole chain works physically: Tracker → Gateway (LoRa, durable custody,
ACK) → phone (BLE / USB, phone ledger, confirmed hand-off, page reclaim) →
backend (exact sealed records, verified ledger, map). Every trust decision
on that path still uses **bench** material:

| Link | Today (bench) | Problem |
| --- | --- | --- |
| Tracker record seal (AEAD) | public SF5L0 test credential (`SF3 PROVISION TEST`) | anyone can read and forge records |
| Gateway custody ACK key | `Sf5l0BenchGrantSource` derives it from the public test root | anyone can make a Tracker release unsent data |
| Gateway ← Edge accept | HMAC with a key derived from the public test root | anyone in BLE range can make the Gateway erase records |
| Phone opens records | public test credential | (see owner decision D1) |
| Phone → backend | one shared bearer token, plain HTTP inside Tailscale | any holder of the token can upload |
| BLE link | open, no pairing | anyone can read held (sealed) records |

## Decisions

- **D0 (earlier, owner):** a device's key is loaded once when it is set up
  (provisioning); the Gateway never opens records; the backend owns device
  keys (`SF5H_BACKEND_VERIFIED_LEDGER.md`).
- **D1 (owner, 2026-10-25): offline map on the owner's phone.** In the
  hills without Internet the owner must see positions. The backend gives the
  owner's enrolled phone the **read key of his own trackers** (the per-epoch
  D2A traffic key, never `K_root`), once, while online; the phone keeps it in
  its protected app storage. Accepted risk: whoever takes that phone can
  read those trackers and could forge their records until they are
  re-keyed (new epoch). This is the explicit "site-local trusted endpoint"
  exception the ADR (§2) leaves for a separate decision; it applies only to
  enrolled owner phones, never to Gateways or relays.
- **D2 (from existing candidates, no new choice needed):**
  - Gateway custody grants are a backend-signed snapshot (OGK1, Ed25519
    issuer signature; SF5K3Y–AC) holding per-tracker scoped
    `K_custody_ack` keys derived by the backend from `K_root`
    (`ORUN_GATEWAY_DURABLE_CUSTODY.md` §6). The Gateway never sees `K_root`.
  - Edge accept is an Ed25519 signature by the enrolled Edge's own key
    (SF5I), verified on the Gateway (CC310 verifier pilot, SF5K3AA). The
    Gateway cannot forge it.
- **D3 (default, revisit at deployment):** the backend moves to the
  owner's Linux server (Coolify), reachable over Tailscale; HTTPS when it
  is exposed beyond Tailscale.

## Slices, in order

Each slice removes one bench dependency and is usable on its own.

### 3A — real tracker credentials (provisioning over USB)
Implemented and physically checked: `docs/milestones/STEP3A.md`.
- Backend credential registry: per device `credential_id` (16 B random),
  `key_epoch`, `K_root` (32 B random), owner. Generated on the backend;
  never logged; file readable only by the backend user.
- Provisioning tool (PC, USB, physical access = the ADR's
  physical-service authorization): writes the credential into the
  Tracker's SecurityStore once (fresh credential lifetime; refuses reuse).
  Replaces `SF3 PROVISION TEST` outside bench images.
- Backend opens records with the registry instead of `--bench-test-key`.
- Check: re-provisioned collar → backend opens its records; the old test
  key and the phone's bench key no longer open them.

### 3B — Gateway enrollment (signed custody grants)
Implemented (bench): `docs/milestones/STEP3B.md` (rollback anchor = the
installed snapshot; documented deviation from SF5K3Z).
- Backend issuer key (Ed25519) and its pinned public key in Gateway firmware
  (or written once at Gateway provisioning).
- Backend builds the Gateway's OGK1 snapshot (its trackers, their scoped
  ACK keys, grant generation) and signs it; the PC tool writes it over USB.
- `GatewayAnchoredGrantSource` replaces `Sf5l0BenchGrantSource`. Open
  gate from SF5K3T/Z: the rollback-resistant revision anchor and the A/B
  append writer (SF5K3AB/AC are simulations).
- Tracker side: its allowed-Gateway list comes from the same provisioning
  step (no bench auto-allow).
- Check: an enrolled Gateway's ACK releases records; a non-enrolled Gateway
  and the bench key do not; revoking the grant stops new ACKs.

### 3C — Edge (phone) enrollment
- The phone creates its own Ed25519 key pair and an Edge id; the owner
  approves it on the backend (one-time code shown in the app).
- The backend then gives the phone: a per-phone access credential for
  uploads (replaces the shared token), and the read keys of the owner's
  trackers (D1).
- The phone's public key goes into the Gateway's signed snapshot (3B), so
  the Gateway accepts hand-offs only from enrolled Edges (SF5I signature
  replaces the bench HMAC accept).
- Check: an enrolled phone hands off and uploads; a second, unenrolled phone
  can read the BLE link but cannot make the Gateway erase anything or upload.

### 3D — link hardening and bench removal
- BLE: bonded, encrypted link for the Edge service (privacy of held
  records; authorization still comes from 3C, never from the bond).
- Backend over HTTPS when it leaves Tailscale.
- Production images contain no test credential, no `BENCH`/`SF3 PROVISION
  TEST` paths; the bench collar is cleaned up and re-provisioned.

## Non-claims

This plan does not approve wire bytes, flash layouts, KDF labels or
signature formats; does not authorize exporting `K_root` from a device; and
does not make a BLE bond an authorization. A compromised enrolled Gateway can
still acknowledge and drop data within its grant (ACK trust anchor, custody
§6); a stolen owner phone exposes that owner's trackers (D1).
