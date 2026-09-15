# Pre-M6 B2 — Portable Core Boundaries

## Baseline

B2 starts from `main` commit `eb5bafe6ed7f01d4ed1b48424b75f7c736d82ee3`, after the B1A compatibility freeze.

B1B physical compatibility evidence was completed before this refactor:

- tracker B: `0E8ADE7E71531AA3` acquired GNSS outdoors and published POSITION;
- base A: `09A462BD4B275BA5` received DIRECT POSITION packets from B;
- observed sequences included `1792`, `2048`, and `2049`.

That evidence is the behavioral reference for B2. B2 must not reinterpret it as a new protocol or storage format.

## Scope

B2 addresses architecture gaps G03–G05 only:

1. extract the GNSS value consumed by core code from the SparkFun callback types;
2. extract device identity from radio initialization while preserving the exact legacy RAK byte ordering and 64-bit value;
3. make legacy POSITION construction a pure compatibility mapping before transport.

B2 deliberately does **not** introduce a general HAL, a second board implementation, a new packet type, a new storage schema, a new role policy, or a new GNSS state machine.

## Boundaries introduced

### Portable GNSS value

`firmware/include/gnss_fix.h` owns `GnssFix`. Core/application consumers can include this value without including the SparkFun u-blox library. `GnssManager` still owns the concrete UBX callback types and recovery state machine.

### Device identity

`firmware/include/device_identity.h` defines the portable `DeviceIdentity` value. Its legacy `uint64_t` representation remains lossless for the current protocol, storage key, duplicate key, and diagnostics.

`deviceIdentityFromLegacyBytes` freezes the existing eight-byte, most-significant-byte-first conversion without Arduino/Nordic/SX126x dependencies.

`firmware/src/rak_device_identity.cpp` is the RAK4630/4631 adapter. It is the only new B2 code which calls `BoardGetUniqueId`.

Production composition now resolves identity before radio startup, injects it into `RadioManager` and `PositionFlow`, and initializes history with the same legacy 64-bit value. Radio failure therefore cannot prevent identity from being available to storage recovery or POSITION mapping.

`RadioManager::begin` retains a RAK-provider fallback only for legacy direct host-test seams. Production `main.cpp` does not use that fallback.

### Legacy POSITION mapping

`firmware/include/legacy_position_mapping.h` and `firmware/src/legacy_position_mapping.cpp` provide a pure mapping:

`GnssFix + DeviceIdentity + sequence -> frozen v1 POSITION bytes`

The mapper owns no sequence allocation, persistence, freshness decision, radio driver access, or timing state.

The active `PositionFlow` now:

1. applies the same fresh-fix age gate;
2. allocates the sequence/ticket from `HistoryStore` exactly as before;
3. maps the value to the frozen 34-byte POSITION representation;
4. appends that exact packet to history;
5. only after successful append, offers the stored bytes to the radio;
6. retains the same live-expiry behavior and leaves expired packets in backlog.

`RadioManager::encodePosition` remains temporarily as a compatibility shim for existing direct host seams, but production `PositionFlow` no longer calls it. Removing that shim is intentionally outside B2 so the portability change does not widen into a test-harness rewrite.

## Compatibility invariants

B2 changes no external format or radio profile:

- protocol version and packet type values: unchanged;
- POSITION size: exactly 34 bytes;
- POSITION field offsets/endianness: unchanged;
- A identity: `09A462BD4B275BA5` unchanged;
- B identity: `0E8ADE7E71531AA3` unchanged;
- sequence ticket semantics: unchanged;
- journal geometry and record format: unchanged;
- store-before-TX rule: unchanged;
- 5-second live freshness/expiry rule: unchanged;
- RAK4630 LoRa frequency/SF/BW/CR/preamble/sync-word configuration: unchanged;
- GNSS freshness, drain/resync, I2C recovery, watchdog, and radio-driver gate state machines: unchanged.

The disabled M1 test-beacon path remains a compatibility path; B2 does not turn it on or define future reporting cadence.

## Host coverage

`firmware/tests/b2/test_b2.cpp` is built without the SparkFun/Arduino host stub include path. It freezes:

- A and B BoardGetUniqueId byte ordering;
- leading-zero and all-ones identity boundaries;
- rejection of malformed identity byte input;
- exact legacy POSITION golden bytes;
- exclusion of local `captured_at_ms` metadata from the wire representation.

The normal host suite also continues to exercise B1A legacy packet vectors, M3/R3 GNSS behavior, M4 journal/store-first behavior, M5 network behavior, R2 radio ownership, startup failure scenarios, and R4 watchdog/I2C behavior.

## Required validation before merge

Run on Debian from the repository root:

```bash
./firmware/tests/run_host_tests.sh
(cd firmware && pio run -e rak4630)
```

Expected result: both commands PASS with no compatibility fixture changes.

Because B2 changes the production composition and POSITION construction path, the final pre-merge hardware sanity check should repeat the essential B1B path: B obtains a real GNSS fix and A receives B's POSITION as `DIRECT`, with the same device IDs. No particular sequence number is expected because the persisted ticket continues from flash state.

## Deferred work

The following are intentionally deferred:

- hiding SparkFun UBX callback declarations from the concrete `gnss_manager.h` itself;
- replacing concrete radio/storage classes with broad abstract interfaces;
- second-board support;
- configuration ownership and migration;
- PHONE/location semantics or mapping PHONE data into legacy GNSS POSITION;
- removal of compatibility shims that would require a wider host-test rewrite.

These should be taken only when a concrete next platform or milestone requires them.
