# ORUN Step 3C — Edge enrollment security preflight (backend-only)

**Status:** IMPLEMENTATION CANDIDATE — **OWNER HOST 17/17 PASS, 2026-10-29, commit `5569bcd`**. Real Android enrollment, production auth and physical Gateway: NOT TESTED.
Owner-approved 3C direction: `docs/architecture/ORUN_STEP3_REAL_SECURITY_PLAN.md`;
security policy: `docs/architecture/ADR_M7P6_SECURITY_ARCHITECTURE.md`.

## What this actually implements

- A separate SQLite security registry (never the live observation ledger).
- A phone/PC installation generates its **own Ed25519 key pair**; only a
  public key is submitted via a future enrollment UI/protected transport.
- `begin(installation_id, public_key)`: pending request, random 32-byte
  one-use challenge (15-minute expiry), public-key fingerprint.
- `approve(request_id, expected_fingerprint, site_id, actor_id, scopes)`:
  **trusted owner/operator action ONLY**, requires independently comparing the
  fingerprint as shown by the candidate installation. Issues a one-time
  random approval secret, not a standing access credential.
- `finish(..., signed_challenge, owner_code)`: real Ed25519 possession proof
  plus owner approval; issues a distinct high-entropy **per-installation**
  access token. Only its SHA-256 digest is stored in the 0600 registry.
- `resolve_headers`: injected principal resolver for S2 SharedStore's HTTP
  adapter; returns only server-stored `Actor(site, user, install, scopes)`.
  Arbitrary `X-Site-ID`, `X-Actor`, or other user-sent claims do not authorize.
- `revoke(installation_id)`: trusted owner administrative operation, disables
  the installation's token without revoking the other enrolled phones.
- Injections are read-only queries into the enrollment registry; Tailscale
  membership / shared bench bearer token are **not** an installation identity.

## What is NOT implemented / security review blockers

- **No public enrollment HTTP endpoints or operator GUI.** `approve` and
  `revoke` must not be wired into anonymous REST or bench-token routes.
- No phone-side Android Keystore key generation/Ed25519 signature/protected
  storage, multi-phone actual enrollment UI, HTTPS deployment or token refresh.
- No audited rate limiting, trial counters, key rotation, enrollment code
  out-of-band transport, signed persistent grant delivery or complete owner
  account recovery. These are release requirements.
- No Gateway Ed25519 grant snapshot update, SF5I Edge custody proof, separate
  per-device authorization, read-key delivery, BLE pairing, firmware or LoRa.
- No production proof from SQLite host tests. Registry DB in a separate
  directory must be owner-only; do not attach as a production backend yet.
- Permission scopes here are *resource-type + site* access only. A
  single-device-restricted helper needs backend Device Registry filtering;
  this prototype must not be deployed as a finished fine-grained ACL.
- The token is a bearer credential: transporting it over unprotected public
  HTTP leaks authority. A secure, authenticated TLS path is required for
  production exposure; Tailscale is current private bench network, **not** a
  replacement for enrollment/authorization.
- Tracker `K_root` is **never exposed** by this component.

## Contract and validation

Focused host runner:
```bash
bash tools/ci/run_step3c_edge_enrollment_host.sh
```

Expected tests: Ed25519 valid/wrong signature, wrong owner code,
unapproved/expired request, fingerprint mismatch, signature replay,
per-install distinct token, session expiry, revocation, unauthorized HTTP,
view-only vs edit, cross-site isolation and server restart. All tests use
temporary SQLite databases and loopback HTTP in host process.

Success must be reported as **host proof of server-side enrollment boundary**,
not a production multi-device login or real Android live sync.

## Owner host verification evidence (2026-10-29)

- `bash tools/ci/run_step3c_edge_enrollment_host.sh`: **17 tests, 17 PASS, 0 FAIL**, 5.207s.
- Python `cryptography` 38.0.4. Checkout HEAD `5569bcd`.
- The suite covered Ed25519 possession proof, fingerprint mismatch, owner approval and revocation, expiry, replay, isolated installations and HTTP permission checks.
- This is **host-only preflight**; no running PC Edge, Backend, Android app, installed device, BLE or LoRa was altered.

## Following milestones

1. Independent security review and fail-closed authentication/audit test
   expansion; code cannot be merged for production without approval.
2. Android per-install protected key store + owner code/fingerprint pairing,
   HTTPS + token rotation/revocation.
3. S2 shared-data API auth integration and real app durable offline outbox +
   collision-proof migration of existing local zone/group/target records.
4. Web/PC same per-site ACL; later one-device view scope.
5. Separate signed Gateway Edge-accept authority and Tracker geofence
   USB/BLE/LoRa mutation authorization.
