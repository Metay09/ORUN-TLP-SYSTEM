# M6D3A — Geofence layout + format/classifier foundation

Status: **IMPLEMENTED ON BRANCH; OWNER HOST/RAK4630 VALIDATION PENDING; NO PHYSICAL PERSISTENCE CLAIM**.

Baseline: `main@5980f06eeb3cb55fcb09aa89ef3565a51c88dcda` (M6D3 architecture docs merged via PR #53).
Branch: `feat/m6d3a-geofence-format-foundation`.
Implementation head before validation docs: `f31a3b2b54cdc8f34c2b5fa0290a3193c1c3dc2e`.

## Purpose

M6D3A freezes and tests the byte-level durable geofence snapshot contract and
reserves its future two-page partition. It deliberately adds **no writer** and
does not activate production geofence runtime.

## Flash layout change

`storage_config.h` now reserves:

```text
0x0E5000..0x0E7000  future GeofenceStore  2 pages
0x0E7000..0x0E9000  SecurityStore         2 pages
0x0E9000..0x0EB000  ConfigStore           2 pages
0x0EB000..0x0ED000  BLE bond InternalFS   2 pages
0x0ED000..0x0F4000  HistoryStore          7 pages
```

`kApplicationPolicyEndAddress` is now `0x0E5000`.

The post-link build guard no longer derives the ceiling from the historical
SecurityStore start. A pure parser validates:

- literal `kGeofenceRegionStart = 0x0E5000`;
- exactly two geofence pages;
- derived geofence end;
- application-ceiling alias to geofence start;
- geofence -> SecurityStore contiguity;
- audited 4096-byte page size.

The host parser regression intentionally proves that leaving
`kFutureSecurityRegionStart = 0x0E7000` in the header cannot make the real build
guard silently use the old ceiling.

## Geofence format v1

Family magic: `"ORG1"` / `0x4F524731`.
Version: `1`.

One snapshot is a fixed **564-byte** sealed record:

```text
offset  size  field
0       4     magic
4       1     version
5       3     reserved = 0
8       8     physical generation (u64, non-zero)
16      8     geofence token incarnation (u64, non-zero)
24      4     geofence token revision (u32, non-zero)
28      1     resource state: CLEAR=0 / CONFIGURED=1
29      1     area_count
30      2     total effective vertex count
32      2     fixed payload size = 520
34      2     reserved = 0
36      8     per-area effective vertex counts
44      512   64 fixed E7 coordinate slots (lat int32 + lon int32)
556     4     CRC-32/ISO-HDLC over bytes [0,556)
560     4     commit word = 0, programmed separately/last by future store
-------------------------------
total   564
```

Unused area-count and coordinate slots are canonical zero bytes. This keeps
CRC/commit offsets fixed for CLEAR and CONFIGURED snapshots.

## Canonical semantics

- maximum areas: **8**;
- maximum total effective vertices: **64**;
- per-polygon effective maximum remains **64**;
- explicit final closing duplicate is removed before persistence;
- polygon and vertex order are preserved; no sorting, winding reversal or
  geometric-equivalence normalization is attempted;
- all configured polygons are revalidated through the existing M6C geometry
  contract before encode and after decode;
- CLEAR contains zero areas, zero total vertices, zero area counts and zero
  coordinate slots;
- CONFIGURED with zero areas is invalid;
- physical generation is independent from semantic token revision.

## Page evidence classifier

The pure classifier distinguishes:

- ERASED;
- STAGED valid body with commit erased;
- UNCOMMITTED_OR_TORN;
- PARTIAL_COMMIT;
- COMMITTED_CLEAR;
- COMMITTED_CONFIGURED;
- COMMITTED_CORRUPT;
- UNSUPPORTED_NEWER;
- SUPPORTED_CORRUPT.

Erased/corrupt/newer evidence is never interpreted as CLEAR.

A pure two-page helper identifies the specifically reviewed contradictory case
where two authoritative committed records carry different non-zero token
incarnations. M6D3B owns full recovery/token-state policy.

## Tests added

`firmware/tests/test_storage_layout_policy.py` covers the real `0x0E5000` build
ceiling and fails closed on stale alias/broken region-chain edits.

`firmware/tests/m6/test_m6d3a_geofence_format.cpp` covers:

- exact CLEAR golden header + CRC;
- maximum 8-area / 64-vertex golden CRC;
- explicit closing-duplicate canonicalization;
- generation/revision separation;
- staged/partial-commit non-authority;
- malformed counts and overflow;
- reserved bytes/payload length;
- CONFIGURED+zero-area rejection;
- unknown state rejection;
- CLEAR-with-hidden-geometry rejection;
- invalid geometry rejection;
- committed CRC corruption;
- erased evidence;
- unsupported-newer versus prefix-torn precedence;
- contradictory committed incarnation evidence;
- invalid generation/incarnation/revision rejection.

The complete host runner includes both new checks.

## Explicit non-goals / compatibility

No changes are made to:

- `main.cpp` production composition;
- `FlashMutationGate`;
- `NrfGeofenceFlash` / `GeofenceStore` (do not exist yet);
- ConfigStore/SecurityStore/HistoryStore formats or pages;
- BLE/LoRa/USB mutation paths;
- TLP v1/v2 bytes;
- RF/relay behavior;
- M6D2 operational confirmation/cadence behavior;
- GNSS freshness/session behavior.

Expected production behavior delta: **none**. The only production build effect
is the stricter application flash ceiling; format code has no runtime caller.

## Validation still required

Before audit/merge:

1. run `./firmware/tests/run_host_tests.sh`;
2. run `pio run -d firmware -e rak4630` and confirm the post-link ceiling guard;
3. record final RAM/flash;
4. independent Astra audit of this code-bearing slice;
5. fix any real findings and repeat affected validation.

M6D3A does **not** require physical hardware testing because it has no flash
writer. Host/build PASS must not be reported as physical persistence or power-cut
PASS. M6D3B owns the first read-only hardware preflight and destructive
persistence qualification.
