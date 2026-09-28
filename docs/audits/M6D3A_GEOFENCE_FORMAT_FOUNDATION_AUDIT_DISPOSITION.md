# M6D3A Geofence Format Foundation — Audit Disposition

Status: **CLOSED / MERGE-READY — 2026-09-28**

Baseline:
`main@5980f06eeb3cb55fcb09aa89ef3565a51c88dcda`

Branch:
`feat/m6d3a-geofence-format-foundation`

Final pre-merge head before this disposition commit:
`94ea014ebb452665e328d49a3372eea3a30c1060`

## Independent audit result

Astra reviewed the code-bearing M6D3A slice and returned:

```text
BLOCKER 0
HIGH    0
MEDIUM  2
LOW     1
recommendation: FIX THEN MERGE
```

The two MEDIUM findings were merge-blocking and were fixed on the same branch.

## Finding disposition

### MEDIUM — noncanonical stored closing duplicate

**FIXED.**

Durable v1 now requires:

```cpp
effectiveGeofenceVertexCount(polygon) == stored_count
```

The geometry layer may still accept an explicit closing duplicate as input, but
canonicalization removes it before persistence. A manually constructed durable
`[A,B,C,A]` snapshot with count 4 is rejected by encode, and a CRC-resealed
record modified into that representation is rejected by decode/classification.

This restores the effective-vertices-only invariant and prevents a second
authoritative representation of the same closing-duplicate input.

### MEDIUM — storage-layout parser trusted comments

**FIXED.**

The application-ceiling policy parser now:

- strips C/C++ line and block comments;
- preserves quoted string/char contents while stripping comments;
- rejects unterminated block comments;
- requires exactly one active declaration for each required literal;
- requires exactly one active required alias/assertion;
- rejects duplicate active declarations;
- rejects conditional layout source rather than guessing;
- rejects commented/disabled required assertions.

Regression tests cover the original stale-comment attack, duplicate declarations,
conditional evidence and commented assertions.

The enforced production application ceiling remains exactly `0x0E5000`.

### LOW — overflow fixture missed cumulative guard

**FIXED.**

The test now constructs one valid 62-effective-vertex polygon, then declares a
second area with the minimum-valid count of 3. The cumulative guard rejects
`62 + 3 > 64` before a second polygon view can be formed.

## Post-fix validation

Owner reran:

```text
./firmware/tests/run_host_tests.sh
  complete suite: PASS
  M6D3A storage ceiling parser: PASS
  M6D3A geofence format/classifier: PASS

pio run -d firmware -e rak4630
  result: SUCCESS
  RAM:   23,584 / 248,832 bytes (9.5%)
  Flash: 250,476 / 815,104 bytes (30.7%)
```

RAM/Flash remain byte-identical to the prior merged M6D2 production image,
consistent with `geofence_format` having no production runtime caller.

## Compatibility

No production behavior change was introduced to:

- TLP v1 or its golden fixtures;
- RF/relay behavior;
- HistoryStore, ConfigStore or SecurityStore formats;
- BLE bond ownership;
- FlashMutationGate runtime ownership;
- Role / Identity / Profile;
- GNSS/R3;
- M6D2 confirmation/cadence;
- PositionFlow store-before-send;
- `main.cpp`.

The production-visible change is the stricter application flash ceiling at
`0x0E5000`.

## Physical evidence boundary

M6D3A contains no GeofenceStore, no nRF geofence flash writer and no production
geofence mutation path.

Therefore this closure does **not** claim:

- physical persistence PASS;
- physical power-cut PASS;
- read-only flash preflight PASS;
- GNSS/geofence field PASS.

Those belong to M6D3B or later physical slices.

## Disposition

**MERGE.**

The 564-byte geofence format v1 and the `0x0E5000..0x0E7000` reservation are
accepted as the persistence-format/layout input for M6D3B.
