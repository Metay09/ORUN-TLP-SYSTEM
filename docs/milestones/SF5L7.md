# SF5L7 — ORUN Edge phone app (Android): Gateway over BLE and USB, phone ledger, confirmed hand-off, map

**Status:** code + host tests + browser UI smoke. **APK build and phone
test: pending (owner).** Bench only for authentication (public test key).
**Base:** `main@940f817` (SF5L5 merged). BLE needs the SF5L6 Gateway
firmware (PR #163); USB works with the SF5L5 firmware already on `main`.

## Why

Step 2 of the agreed order: the phone becomes the Gateway's Edge. In the
field the owner walks with the Gateway; the phone takes its records, keeps
them durably, and only then tells the Gateway it may free their space
(SF5L5 confirmed delivery). BLE and USB are both required.

## What

`edge/app`: Capacitor 8 + React + Vite (the owner's stack), Android.

- **Core** (`src/core`, pure TypeScript, host-tested): a port of the PC
  tool and of the Python reference ledger.
  - `product.ts`: SF5B PRODUCT_SECURE open (HKDF + AES-128-CCM), the
    canonical PERIODIC/EVENT checks, readable fields.
  - `crypto.ts`: AES-CCM built from AES-CBC (CBC-MAC) + AES-CTR of the
    audited `@noble/ciphers` (WebCrypto has no CCM); HKDF/HMAC/SHA-256 from
    `@noble/hashes`.
  - `edgeLink.ts`: BLE framing/messages (SF5L6), USB lines (SF5L1/SF5L5),
    the Edge accept MAC.
  - `ledger.ts`: verified observations, first exact frame wins, conflicts
    kept as evidence and quarantined, read back after COMMIT.
  - `edge.ts`: one accept in flight; queued only after the ledger COMMIT
    returned and only for STORED/DUPLICATE; BUSY or 3 s silence → resend;
    OK/UNKNOWN → done; REJECTED/FAIL → reported, the record stays on the
    Gateway. All Gateway events are applied strictly in arrival order.
  - `points.ts`: the map JSON, same as the PC tool's `/api/points`
    (newest-by-record-time rule, live/through/cut classification).
- **Links** (`src/platform/transports.ts`):
  - BLE: `@capacitor-community/bluetooth-le`. System picker filtered by
    the ORUN Edge service UUID, remembered for the next start; high
    connection priority; indications in, write-with-response out;
    reconnects every 5 s after a drop.
  - USB OTG: `@leeskies/capacitor-usb-serial` (wraps the mature
    `usb-serial-for-android`). RAK4631 CDC-ACM `239A:8029`, 115200, DTR on.
    Sends `BENCH GW DUMP` at connect. Android offers to open the app when
    the Gateway is plugged in.
- **Phone ledger** (`src/platform/sqliteDb.ts`):
  `@capacitor-community/sqlite`. WAL + `synchronous=FULL`, explicit
  transactions; the status line shows what SQLite reports.
- **UI**: Bağlantı tab (Bluetooth / USB / other gateway / disconnect,
  counters, record stream) and Harita tab. The map is the same page as the
  PC tool (`tools/edge_map/index.html`, copied at build): one map, two
  hosts. The page now takes its data from the phone ledger when it runs
  inside the app, and the app refreshes it as records land.

Also fixed here (and in PR #163): the PC tool did not recognise the
Gateway's real reject line `EDGE ACCEPT REJECTED reason=MAC page=P slot=S`,
so a rejected record would have been resent every 3 s forever.

## Evidence

- `test/core.test.ts` (`node --test`): AES-CCM against RFC 3610 packet
  vector #1 and every single-bit tamper; the shared BLE frame vector; every
  message size 1..160; gap / foreign id / bad version / oversize dropped,
  3 s expiry; the shared accept-MAC vector (`EE0E6E26…`); SF5B KAT frames;
  the ledger (stored, duplicate, rejected, conflict quarantine, commit
  failure → UNCERTAIN → retry, damaged stored frame → UNCERTAIN); USB
  session (RX signal pairing, accept after commit, BUSY/timeout retry,
  OK/REJECTED reason=…, truncated line); BLE session (HELLO first, link,
  binary ACCEPT, results, reclaim, dump end, session end); map
  classification.
- `test_sf5l7_edge_app.py`: the same inputs through the Python reference
  and the TypeScript core. 3018 frames (authentic frames with random
  plaintext mutations + tampered ciphertext; 2101 accepted, both sides
  agree on every one, byte for byte), 200 random accept MACs and lines, and
  a 75-line USB session. Every printed line, every accept written, the
  summary and the map points are identical.
- Typecheck (`tsc --strict`) and `vite build` PASS.
- Browser smoke (Chromium, 390×844, sql.js in place of the phone SQLite):
  simulated Gateway pass of 12 records → counters, record stream, map with
  12 points and the latest-position card.
- Not possible here: the APK build (no Android SDK / Google Maven in this
  workspace). The owner builds it.

## Known limits

- Bench security: the phone opens records with the PUBLIC test
  credential and signs accepts with the public test key. The reference
  ledger is backend-side by design; the phone decrypting is a bench step.
  Step 3 decides Edge credentials.
- Foreground only: Android may pause the app when the screen is off. A
  foreground service (keep the link while walking) comes later.
- Map tiles need Internet (OpenStreetMap / satellite). Offline tiles later.
- No phone → backend sync yet (MQTT/HTTPS), next step.
- The USB plugin is a young third-party package (0.2.0). It wraps the
  mature `usb-serial-for-android`; the link code keeps it behind one class
  so it can be swapped.
