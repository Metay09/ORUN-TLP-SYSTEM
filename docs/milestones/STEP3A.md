# Step 3A — real Tracker credentials (USB provisioning, backend registry)

**Status:** code + host tests. **Physical check: pending (owner).**
Plan: `docs/architecture/ORUN_STEP3_REAL_SECURITY_PLAN.md`.
No wire, packet or flash format changes: frames, ACKs and stores are the
same; only the keys behind them are real now.

## What changes

| Link | Before | After 3A |
| --- | --- | --- |
| Tracker seal | public test credential | its own random credential from the backend registry, written once over USB |
| Gateway custody ACK key | derived from the public test root | scoped key the backend derives from that Tracker's root, written over USB (RAM only until 3B) |
| Phone opens records | public test credential | per-epoch read key from the backend (owner decision D1) |
| Backend opens records | public test credential | registry (test key only for old bench records) |
| Edge accept, BLE, phone → backend auth | unchanged (3C / 3D) | |

## Pieces

- `backend/orun_credentials.py`: registry (SQLite, created 0600).
  - Random 16-byte credential id and 32-byte root (`secrets`).
  - Epochs only grow per device and skip the test epoch, so
    (device, epoch) always names one credential lifetime.
  - Retired credentials stay readable for old records; ids and roots are
    UNIQUE and never reintroduced.
  - Derivations: scoped custody ACK key (same HKDF as the Tracker verifier),
    D2A traffic key (what the phone gets), and the two USB lines.
- `tools/orun_provision.py`:
  - `tracker`: reads `BENCH?`, refuses a collar holding another credential
    (e.g. the test key → CLEAN ALL first), sends `BENCH PROVISION`, waits for
    `BENCH SETUP PASS`, checks the collar's report matches the registry and
    records the grant scope it reports.
  - `gateway`: writes every recorded grant (`BENCH GRANT`).
  - `new`, `list`: never print keys.
- Tracker bench image:
  - `BENCH PROVISION <cred> <epoch> <root> <gateway> <crc>` → the existing
    provisioning sequence (commit credential, v3 migration, grant Gateway)
    with the real credential; idempotent for the same credential; any other
    credential is never overwritten.
  - `BENCH?` adds `BENCH CREDENTIAL credential=… epoch=… grant_generation=…
    custody_floor=…` (public facts, never the root).
  - The pilot TX is allowed for any provisioned credential with a granted
    Gateway (was: test key only).
  - The command buffer and the parsed key are wiped.
- Gateway bench image: `BENCH GRANT <tracker> <epoch> <floor> <generation>
  <key> <crc>` → RAM grant table (8 entries), checked before the test-key
  fallback; `BENCH?` shows `ram_grants=n`. **Lost on reboot**: run
  `orun_provision.py gateway` again (the Tracker keeps its records meanwhile).
- `rak4630_m7p6f_security_cleanup`: new `CLEAN ALL` also erases the 128 KiB
  product record region. Records sealed under a retired credential can
  never be released afterwards, so they must not stay in the ring.
- Backend server: `--registry` (default `~/orun-credentials.sqlite`);
  `GET /v1/edge/keys` (token) returns read keys only.
- PC tool: `--registry` too.
- Phone app: `KeyRing` (table `read_keys`).
  - Fetched with the server settings (every 10 min, on "Şimdi gönder").
  - New keys → the Gateway is asked for its held records again: records the
    phone could not open were never accepted, so they are still on the
    Gateway.

CRC-32 in the USB lines is a typo guard only; USB physical access is the
authorization (ADR M7P6 §11).

## Evidence

- `test_step3a_credentials.cpp` (ASan/UBSan, real SecurityStore + Gateway
  ingress + real Tracker verifier):
  - line parsers against the backend vectors (exact, typo → bad CRC,
    malformed);
  - blank Tracker gets a real credential, v3 + Gateway grant (generation 1,
    floor 0, as the backend assumes); idempotent; other credential and the
    test key refused, flash untouched;
  - backend-derived RAM grant → ACK authenticated by the real Tracker
    verifier; no grant → no custody; wrong key → not authenticated; test-key
    ACK for an old frame → not authenticated by the re-provisioned Tracker;
    table limits;
  - the whole SF5L0 bench test-key suite still passes.
- `test_step3a_credentials.py`:
  - registry rules;
  - vectors equal the C++ test's lines;
  - provisioning tool against a scripted serial device (blank, re-run,
    test-key refusal, grant replies);
  - backend: registry opens real records, the bench key alone does not;
    `/v1/edge/keys` needs the token and never contains a root;
  - **end to end with the app core (Node)**: the phone fetches its read keys
    from the running backend, opens 3 real-credential records it could not
    open before, and uploads them.
- `keys.test.ts`: key ring, persistence, wrong key, BLE ingest before and
  after the keys.
- Full host suite PASS; `rak4630` unchanged (362 912 B); Tracker bench,
  Gateway bench and cleanup images build.

## Known limits

- Gateway grants are RAM only here; step 3B (`STEP3B.md`) makes them signed
  and persistent (`orun_provision.py gateway`; `--ram` keeps this path).
- Edge accept still uses the bench HMAC key; phone → backend still uses the
  shared token (3C).
- The registry is a plain file on the owner's machine (0600); no key
  wrapping/hardware protection yet.
