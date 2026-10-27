# ORUN: latest location before background Gateway history (27 Oct 2026)

**Owner's field observation:** With phone connected to Gateway, the oldest
locations arrive first. The current location is buried at the end of a
long transfer. The owner wants the map's newest already-available location
early, while historical records still drain chronologically in background.
Owner also verified **Esri current + source zoom cap 18** works best.

**Status:** source implementation + new regression cases authored, NOT YET
validated by host build and NOT flashed to Gateway or tested on phone.

## Why this happens

- `sf5l0ForEachHeld` sorts durable Gateway custody by ascending
  `(page generation, slot)`, and both USB DUMP and BLE `nextHeld()`
  report history in that order. This is physical Gateway custody order,
  not necessarily measurement timestamps: Gateway does not decrypt.
- `edge/app/src/app/store.ts` previously *debounced* every update:
  during long bursts, each version increment cancelled the previous
  pending read. It could defer the map until transfers became quiet.
- `herd.ts` already displays the newest *recorded measurement time* per
  animal regardless of ingest order. Backend upload already uses newest
  phone-ledger row first; neither of those data contracts should change.

## Compatible transfer design

New Gateway firmware sends **one newest durably admitted PERIODIC frame**
as a custody head-preview, followed by the entire normal oldest-first
archive. A new incoming PERIODIC during backlog gets another one-message
preview promptly; the archive cursor is never rewound. The preview is
**NOT** a source of truth about the encrypted measurement timestamp and
cannot replace the normal history walk.

- BLE `0x06 HELD_PREVIEW`: exact same envelope `0x02 HELD`, except
  type. Old phone clients ignore unknown `0x06` and continue to receive
  the ordinary full `0x02` archive.
- USB `EDGE PRIORITY v1 ...`: same exact object/custody handles as
  `EDGE HELD v1 ...`, emitted immediately before the ordinary DUMP.
  Old clients ignore the priority line; old HELD and DUMP END unchanged.
- No change to the authenticated HMAC ACCEPT message, durable-commit
  verification, radio custody ACK, or gateway reclaim; latest preview
  gets full phone ledger AEAD/authentication + disk commit before ACK.
- Preview announcements are NOT counted toward `DUMP END count=N`.
  The phone excludes them from `dumpSeen`, while regular chronological
  HELD maintains the original census; this prevents false or optimistic
  "everything transferred" counts.
- A frame's *visible untrusted* outer family tag is used **only** to
  prioritize a PERIODIC frame rather than an EVENT. The phone verifies
  the full sealed frame before display or ACK.

## UI and map update

- `usePoints` now uses a leading/trailing max-once-per-2-second
  scheduler: incoming versions mark the data dirty without cancelling a
  running timer. A busy Gateway can no longer indefinitely suppress
  map refresh.
- `buildHerd` continues to choose last record by authenticated
  measurement time. An archived older record never rolls the animal
  marker backward.
- New installs default to `sat={kind:esri,maxZoom:18}`. Owner's saved
  preferences (including any earlier explicit safe source cap) win.

## Limitations (do not overpromise)

1. **Gateway firmware update required** for any newest-first effect.
   Installing the new APK onto a Gateway with old firmware fixes UI
   refresh starvation, but cannot reorder bytes the old Gateway sends.
2. Priority means **newest custody admission**, not guaranteed newest
   actual GNSS sample. A fresh location still on the Tracker or in RF
   relay queues cannot appear on the map before Gateway receives it.
3. One newest PERIODIC preview per Gateway sweep currently. When many
   collars share a gateway, some animals must still wait for their
   corresponding background historical record. A future bounded
   per-collar latest index could make this complete.
4. This Gateway firmware composition is the **SF5L0 bench path using
   PUBLIC TEST KEYS**. Never flash it over real provisioned production
   hardware or treat this as a shipping firmware image.
5. Radio duty limits, sample interval and production secure EVENT
   transport are untouched.

## Required independent validation

On an isolated worktree at this commit, before Android install/firmware:
```sh
bash tools/ci/run_sf5l6_ble_edge_host.sh
bash tools/ci/run_sf5l0_bench_round_trip_host.sh
cd edge/app
npm test
npm run build
cd ../../firmware
pio run -e rak4630
pio run -e rak4630_sf5l0_gateway_bench
```
Gateway bench env requires the existing issuer public key file; an
unavailable key is a build environment issue, never fabricate a key.

Test: connect new phone to updated bench Gateway containing 40 old
PERIODICs, observe 0x01 HELLO -> 0x06 head -> 0x02 chronological x40 ->
0x03 DUMP_END count=40. Insert new live PERIODIC midway through the
archive and require another 0x06 ahead of the remaining backlog.
Confirm no skipped records, same HMAC accept and reclaim, and that map
updates before archival completion. Test USB PRIORITY similarly. Revert
to old Gateway firmware and verify old HELD still works.
