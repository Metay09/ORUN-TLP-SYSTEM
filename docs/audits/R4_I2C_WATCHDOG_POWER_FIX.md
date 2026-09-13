# R4 — bounded I2C, watchdog and switched-rail ownership

Base: `b6d23b7` on `fix/r4-i2c-watchdog-power`.

## Problem

Adafruit nRF52 Arduino core 1.7.0 implements master `Wire.requestFrom()` and
`Wire.endTransmission()` with unbounded polling loops for TWIM events such as
RXSTARTED, LASTRX, TXSTARTED, LASTTX, STOPPED and SUSPENDED. If a target or bus
stalls, the cooperative ORUN loop can remain inside `Wire` forever; the 120 s
GNSS acquisition timeout then cannot execute.

R3 also added direct u-blox bytes-available reads, so all GNSS I2C operations
must share the same bounded failure policy.

## R4 policy

### Pinned Wire transform

`firmware/scripts/patch_wire.py` validates the exact Adafruit nRF52 1.7.0
`Wire_nRF52.cpp` Git blob (`c3e2df7001619d242c7f82acd447c2a7cb32a63b`).
It replaces the master TWIM event waits with bounded waits:

- 25 ms event deadline;
- independent finite spin ceiling if the software millisecond clock itself
  stops advancing;
- bounded STOP attempt on timeout;
- TWIM event/error cleanup and peripheral disable/re-enable;
- sticky `orunWireTakeTimeoutFlag()` handoff to ORUN application code.

After one timeout, later master operations fail quickly until ORUN consumes the
flag and performs bus recovery. The transform is fail-closed and source-version
pinned. During normal PlatformIO execution the framework source is restored at
process exit; an original backup allows validation/recovery after an interrupted
build.

### GPIO bus recovery

`I2cRecovery` consumes the timeout flag, detaches TWIM, releases SDA/SCL, and:

1. verifies SCL can rise within a bounded wait;
2. clocks SCL at most nine times while SDA remains low;
3. emits a STOP condition;
4. verifies both lines are released;
5. restores `Wire` at 100 kHz.

No recovery path contains an unbounded wait. A permanently stuck SCL/SDA returns
failure instead of looping.

### GNSS integration

A Wire timeout can occur during detection, configuration, STARTING drain,
ACQUIRING polling, R3 transport resync, or continuous-tracking IDLE drain.

- Detection keeps the existing three-attempt policy and records I2C diagnostics.
- During an active acquisition a successful bus recovery increments the GNSS
  session generation, clears every PVT/DOP/freshness boundary, reapplies GNSS
  configuration, and passes through the R3 STARTING drain before data can be
  accepted again.
- The original acquisition timeout anchor is retained, so repeated recovery
  cannot extend one acquisition forever.
- At most two in-acquisition I2C recoveries are permitted. Further timeout or a
  failed GPIO recovery enters fail-closed `kFailure`; no stale position can be
  promoted.
- A fault while IDLE starts one bounded recovery acquisition immediately.

### Hardware watchdog

`WatchdogManager` starts the nRF52840 hardware watchdog early in `setup()` with
a 30 s timeout. It runs during normal CPU sleep and pauses while the debugger
halts the CPU. The watchdog is fed only after the cooperative application loop
has completed its GNSS/storage/radio work. A path which truly never returns
therefore resets the MCU even if a lower-level time source or driver is wedged.

Boot logs include the nRF RESETREAS value and whether the previous reset was a
watchdog reset. The watchdog is a final fallback, not a substitute for the
bounded Wire and bus-recovery paths.

### Shared WB_IO2 / 3V3_S ownership

GNSS no longer writes `WB_IO2` directly. `SensorPowerManager` owns the switched
rail with an owner bitmask. GNSS acquires/releases only its own owner bit, so a
future second 3V3_S consumer can keep the rail powered independently.

RAK1904/LIS3DH itself is not assumed to be powered from this switched rail.
M6 must still respect slot/pin conflicts (notably use of WB_IO2 as an interrupt
on some WisBlock slot combinations) instead of treating RAK1904 as a 3V3_S
consumer without hardware evidence.

## Tests

Host coverage adds:

- exact pinned Wire transform / fail-closed source guard;
- no-timeout, recoverable SDA hold, and permanently stuck SCL recovery cases;
- central rail ownership and idempotent owner acquisition;
- GNSS timeout -> session restart behind freshness boundaries;
- recovery failure and per-acquisition recovery-budget fail-closed behavior;
- watchdog API/timeout policy compilation on host.

All previous M3/R3 host tests compile production `GnssManager` together with
production `I2cRecovery` and `SensorPowerManager`.

## Physical validation still required

Compilation and host fault injection do not prove electrical behavior. On real
RAK4630 + RAK12500 hardware verify:

- force SDA low and SCL low separately and measure bounded return/reset behavior;
- inject a stall during SparkFun read/configuration and verify no stale POSITION
  is emitted after recovery;
- verify nine-clock + STOP recovery on a recoverable target hold;
- verify watchdog reset and RESETREAS reporting with an intentional loop stall;
- verify WB_IO2/3V3_S behavior with the actual RAK19007 slot population;
- repeat GNSS power cycles and R3 backlog tests after recovery.

R4 does not claim physical validation until those tests are performed.
