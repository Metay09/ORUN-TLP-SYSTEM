# ORUN Alanlar — hedef seçimi ve yerel USB yükleme adayı

Status: **HOST VALIDATED 2026-10-29 (73/73 Android-host tests, TypeScript, Vite build, 7/7 Python USB-preparation tests PASS); physical Android + USB NOT YET VALIDATED.**
Depends on PR #184 unified Android map. Owner decision: **do not implement
30-day geofence what-if/simulation**. This patch does not implement that feature.

## Existing functionality reused
- Draw/edit/drag/undo/redo; E7 polygon checks and 8/64 limit; GeoJSON import/export.
- No app-generated OUTSIDE alarm. The Tracker remains the authoritative alarm owner.
- Firmware local physical USB parser and one GeofenceMutationOwner:
  APP FENCE BEGIN / V <latitude> <longitude> / AREA / COMMIT.

## Added
- Local explicit intended targets by individual device, named groups, or
  all currently known devices. A new/imported zone targets **nobody** by
  default. Assignments are independent of current GPS position.
- Per-device full replacement set assembled from selected active zones;
  invalid/over-capacity sets block export. Empty sets are **not CLEAR**.
- Fail-closed phone storage key `orun.app.zoneTargets.v1`. Invalid existing
  data is retained and writes blocked. Write errors cannot yield a 'saved'
  confirmation. Original zone geometry storage is unchanged.
- Per-device GeoJSON export from Alanlar.
- `tools/orun_fence_usb.py`: read the exported GeoJSON and verify geometry
  against the existing E7 rules before generating the USB staging sequence.
  Default mode prints a preview; `--apply` needs an explicit Tracker
  `/dev/serial/by-id` port and the 16-hex USB board ID. Exact staging
  counts must be returned at each step; COMMIT success requires
  APPLIED/UNCHANGED + CONFIGURED + expected counts + VALID token, then
  APP GEOFENCE? status corroborates state/counts.

The USB result does **not** prove a byte-for-byte geometry readback (the
current APP GEOFENCE? response exposes counts, not the polygon bytes or digest).
It is a physical USB, operator-authorized bench/service writer; this is
neither authenticated remote apply nor Android direct USB-to-Tracker.

## Important boundaries
- Group/individual selection is a *local plan*, not an applied or delivered
  state. Edits to group membership may change planned targets; no device
  changes until an explicit per-device operation.
- Do not mix this transfer with the running PC Edge Gateway USB connection:
  it is for a **separate Tracker USB port**, not the Gateway.
- Production BLE/LoRa geofence mutation still requires authenticated
  application authorization, fragmentation, CAS, per-device RESULT and
  reconciliation. This patch must not add 'applied' UI badges.
- No automatic CLEAR on disabling/removing a local zone, and no silent
  clearing of an existing physical collar fence. Explicit CLEAR remains a
  separate authorized operation.
- No hardware validation, browser test or complete app CI is claimed here.

## Host validation evidence (owner Debian, commit `d4cdfde`)

- `npm test`: **73 passed / 0 failed**.
- `npm run typecheck`: **PASS**.
- `npm run build`: **PASS** (nonblocking bundle-size warning).
- `python3 firmware/tests/m6/test_orun_fence_usb.py`: **7 passed / 0 failed**.
- No claim of physical Android use, direct USB apply, BLE or LoRa mutation.

## Validation gate
```bash
cd edge/app
npm ci
npm test
npm run typecheck
npm run build
cd ../..
python3 firmware/tests/m6/test_orun_fence_usb.py
```

Physical operation (after host validation), on the *Tracker*, not Gateway:
```bash
python3 tools/orun_fence_usb.py /path/to/orun-fence-TRACKER.geojson
python3 tools/orun_fence_usb.py /path/to/orun-fence-TRACKER.geojson \
  --apply --port /dev/serial/by-id/usb-RAKwireless_WisCore_RAK4631_Board_<TRACKER_BOARD_ID>-if00 \
  --expected-board-id <TRACKER_BOARD_ID>
```
No automatic upload runs from the phone. Before public release, verify on
Android that target drafts survive relaunch, that old zone drafts survive the
PR #184 upgrade and that enabling a target never presents an applied-state
claim. Track remote/Android writer in a separately audited security slice.
