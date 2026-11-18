# KQ05 Gateway legacy-History -> receipt journal candidate (PC-only)

**Status: PORTABLE FORMAT ONLY — no physical partition cutover, release or firmware flash.**
Baseline: PR #332, commit `98f56f22338eb8dcb16baa5346a729098022fe3e`.

## Decision and scope

The owner explicitly permits discarding **old** HistoryStore records. That is
not permission for automatic Flash erasure, nor evidence that every physically
installed Gateway is running the KQ05 role and has no currently pending
legacy custody. No A/B full-page duplication is required or proposed.

- Current physical owner remains the legacy HistoryStore spanning
  `[0xED000,0xF4000)` (7 x 4096 bytes = 28 KiB).
- Candidate **Gateway only**: `[0xED000,0xEF000)` (2 x 4096 = 8 KiB),
  64-byte page headers, 28 x 144-byte fixed-length records on each page.
  Exactly **56 single-copy offers** fit at maximum capacity in this format.
- `[0xEF000,0xF4000)` remains reserved/unmodified (20 KiB). This is
  **not** 20 KiB of separately allocated or writable user Flash.
- Do not change the 128 KiB Product region, Security/OGK1, Config, Geofence,
  InternalFS, or bootloader; this design cannot trade existing custody
  persistence for convenience.
- The Tracker uses Product OBS1 storage for KQ05 periodic/event records.
  The Gateway's 128 KiB Product custody region is not repartitioned.

## Format being proven on PC

`kq05_gateway_receipt_journal_format` only encodes/inspects:
- Per-page owner magic/version, Gateway ID, generation, CRC, and commit-last
  4-byte word; 64-byte header.
- One full logical BLE `0x91` offer (96..124 bytes), with all original
  Edge/Gateway authentication bytes, packed in a 144-byte slot:
  8-byte prefix + 124-byte padded offer + 4-byte CRC + 4 erased reserved
  bytes + 4-byte commit-last marker.
- Erased vs incomplete vs committed vs corrupt is distinguished. A completed
  marker without a valid CRC/shape cannot authorize STAGED. The codec alone
  cannot authorize STAGED **even when fully valid**: it does not verify HMAC
  credentials or prove that a physical Flash write/readback completed.
- Record count, location and bounds are computed using the current
  `storage_config.h` history geometry, with compile-time assertions.
- Physical Flash is never accessed by this module or its host test.

**No A/B page mirroring.** Two append-only pages contain **different**
receipts. Retrying an offer must compare authenticated exact contents and
reuse the already durable record, never create repeated custody credits.
Torn slots may be abandoned. No still-needed receipt may be evicted to make
space. A full journal must refuse STAGED and request retry/backoff at Edge.

## Conditions for an actual Gateway Flash owner — NOT YET MET

1. Explicit, recoverable, **operator-approved** legacy-History migration with
   a backup/clear warning and a separate device-role gate. KQ05 Gateway
   must stop legacy `HistoryStore::begin()/poll()` and all its writers
   **before** exposing those 2 pages to the new owner. Unknown/conflicting
   roles fail closed. No implicit erase or claiming an old History magic
   as an empty receipt page.
2. A separately bounded FlashMutationGate port, sharing the *same* Nordic
   physical flash arbitration and SoftDevice event owner. The new port can
   read/program/erase **only its 8 KiB**, with correct async completion,
   verified readback, quarantine/recovery after power loss and no overlap
   with reserved 20 KiB. The portable format code cannot substitute for
   this.
3. True Gateway-local store: verify authentic Edge installation identity,
   Gateway/Tracker scope, exact HMAC offer and Backend receipt binding;
   only after committed physical NOR readback produce HMAC-protected
   BLE `0x08` + inner `0x92` STAGED. STAGED is **not** Tracker delivery.
4. Write-ahead/torn-slot recovery and bounded duplicate handling on reboot,
   distinct from stale, foreign, unsupported and corrupt page evidence.
   Full/tampered/flash-busy states never send a success reply or erase
   still-needed records.
5. Reclaim one page only when **every** entry in that page is independently
   proven terminal by the agreed authenticated Tracker delivery/reissue
   policy. No periodic blind erase. Physical 10k+ cycle stress / cut-power
   / SoftDevice contention tests before release.
6. End-to-end dedicated RF receipt send (not the legacy command API), legal
   regional budget, ACK/CMD/LIVE priority and Tracker RX window gating;
   real two-device on-air receipt/reboot validation.

## PC qualification

Run `bash tools/ci/run_kq05_receipt_journal_format_host.sh` on a clean
branch. The full `run_kq05_lab_preflight.sh` includes the portable gate.
Until physical owner steps 1–6 pass, existing firmware must continue to
refuse KQ05 `0x91` offers in the Gateway entrypoint. No claim of field
readiness or "8 KiB hardware capacity tested" is permitted.
