# SF5L8 — Edge → Backend upload (phone → bench backend), backend map

**Status:** code + host tests (incl. app core ↔ real backend end to end).
**Phone ↔ server test: pending (owner).** Bench only for authentication
(public test key, shared bearer token over Tailscale).
**Base:** `main` with SF5L7 (#164, #165).

## Why

Completes the chain Tracker → Gateway → Edge (phone) → Backend. The
architecture keeps device keys on the backend only
(`SF5H_BACKEND_VERIFIED_LEDGER.md`: `K_root` must never reach a Gateway,
Edge or phone), so the phone forwards each record's **exact sealed frame**;
the backend authenticates and stores it.

## Choice: HTTPS request/response, not MQTT (for now)

Each upload needs a per-record durable answer before the phone may mark the
record uploaded. A request/response call gives that directly; MQTT would
need a broker plus a separate answer topic. MQTT can be added later for live
push. On the bench the address is the PC's Tailscale IP over plain HTTP
(WireGuard encrypts the path); a real deployment uses HTTPS.

## Protocol (`POST /v1/edge/batch`)

Request (`Authorization: Bearer <token>`, JSON, ≤256 KiB):

```json
{"edge": "phone-1a2b3c4d5e6f",
 "records": [{"frame": "<hex, 84 or 100 bytes>", "gateway": "09A462BD4B275BA5",
              "first_seen_utc": 1793000000, "rssi": -91, "snr": 6}],
 "sessions": [{"id": 3, "started_utc": 1793000000, "alive_utc": 1793000900, "transport": "ble"}]}
```

- ≤200 records, ≤64 sessions; anything malformed → 400 and nothing stored.
- Response: `{"results": ["STORED"|"DUPLICATE"|"CONFLICT"|"REJECTED"|"UNCERTAIN", …], "sessions_ok": true, "records": N}`.
- STORED / DUPLICATE: durable on the backend (verified ledger, readback
  after COMMIT). CONFLICT: same identity, different content, kept as
  evidence. REJECTED: not authentic for a known credential. These four are
  final on the phone. UNCERTAIN, any HTTP error, timeout or bad answer →
  nothing is marked, sent again later.
- An empty batch is the app's address/token check.
- Facts for the map: the earliest Edge arrival and the first known Gateway
  RX signal per record; Edge sessions upserted per (edge, session).

## Backend (`backend/orun_backend_bench.py`)

Python stdlib HTTP + the existing backend verified ledger
(`backend/candidates/product_secure_verified_ledger.py`). It serves the same
map (`tools/edge_map`) over all uploads: `GET /`, `GET /api/points`; plus
`GET /v1/health`. One batch at a time (lock). Bind to the Tailscale address.

## Phone (`edge/app`)

- `src/core/sync.ts` (`Uploader`): newest records first (so the backend map
  shows "now" first), 100 per batch, drained until done; answers recorded in
  `uploads`; a stable random Edge id in `meta`. All database work goes
  through the core's ordered queue; the HTTP call itself does not hold it.
- `src/platform/http.ts`: `fetch` carried by Capacitor native HTTP
  (`CapacitorHttp`), 20 s timeout; `usesCleartextTraffic` for the bench
  Tailscale address.
- UI: "Sunucu" card (address, token, automatic upload, "Şimdi gönder",
  uploaded / pending / conflict / rejected, last contact). Automatic upload
  every 30 s and 5 s after new records.

## Evidence

- `test/sync.test.ts`: newest-first exact frames + local facts; final vs
  UNCERTAIN marking; network / 401 / 500 / wrong-length answers mark
  nothing; drain in batches of 100; ping.
- `test_sf5l8_backend_sync.py`: batch rules (stored, duplicate, rejected,
  conflict, earliest arrival, session upsert, malformed input stores
  nothing); HTTP (401, 413, 400, 404, empty batch, map, points, health);
  **end to end**: the app core fills a phone ledger over BLE messages (130
  records), uploads them with `fetch` to the backend running for real, a
  second run sends nothing, and the backend map points equal the phone's
  (live / through / cut / unknown included).

## Known limits

- Bench security: shared bearer token, public test key, plain HTTP inside
  Tailscale. Edge identity, per-Edge credentials and an authenticated
  BACKEND_DURABLE receipt are step 3.
- The backend's answer does not travel back to the Tracker (no
  BACKEND_DURABLE receipt to devices yet).
- The phone keeps every record (no deletion after upload).
