# R3.2 — partial I2C drain freshness recovery

Base: `5eeac03` on `fix/r3-gnss-freshness`.

## Why R3.1 was still incomplete

R3.1 rejected a stale PVT when either the callback iTOW differed from SparkFun's
newest parsed PVT cache or the PVT callback gap reached the 5 s freshness
limit. An independent verification found a remaining transport sequence:

1. I2C service is delayed long enough to build receiver backlog.
2. `checkUbloxI2C()` parses one old PVT/DOP pair, then a later chunk read fails.
3. SparkFun still dispatches the already-retained callbacks even though
   `checkUblox()` returned false.
4. The first stale PVT triggers R3.1, but R3.1 renewed its local callback time.
5. A second partial read 100 ms later can dispatch the next old pair with a
   matching callback/current iTOW and a short local callback gap.

That second old pair could therefore look fresh even though its receiver
measurement was already older than the freshness limit.

## R3.2 policy

A backlog witness no longer rejects only one callback. It enters a transport
resynchronization gate. While the gate is active, no PVT or DOP callback can
create a candidate. The gate remains closed until a forced SparkFun read
completes successfully *and* a direct read of the u-blox bytes-available
register proves that the receiver output queue is empty.

The two backlog witnesses remain:

- callback PVT iTOW differs from the newest PVT cache iTOW after `checkUblox()`;
- the interval since the previous trusted PVT callback is at least
  `kFreshFixMaxAgeMs` (5000 ms).

The triggering callback does not renew the trusted callback clock.

## Reliable drain confirmation

During resynchronization, at most one forced drain attempt is made every
`kI2cPollingWaitMs` (100 ms):

1. temporarily set SparkFun I2C polling wait to zero;
2. call `checkUblox()`;
3. restore the 100 ms polling wait;
4. dispatch and discard callbacks while the resync gate is still active;
5. only if `checkUblox()` returned true, read u-blox register `0xFD/0xFE` using
   the same bytes-available convention as SparkFun 2.2.29;
6. leave resync only when that status transaction succeeds and reports zero
   queued bytes.

A `checkUblox()==false` result never completes recovery, even when a following
status query would show zero bytes. This matters because SparkFun uses false for
both zero-data polling and transport/partial-read failure.

The status read masks the same undocumented high bit that SparkFun masks. It
does not consume the receiver data stream. It uses the existing `Wire` bus and
therefore does not claim to solve the R4 bounded-I2C / watchdog problem.

## Post-resync boundary

Successful queue drain still cannot prove that SparkFun's byte parser contains
no partial UBX frame from a previous failed chunk. Therefore R3.2 resets both
PVT and DOP boundary markers when recovery completes. The first PVT epoch and
the first DOP epoch after resync are boundaries only; a later matching epoch is
required before a fresh fix can be promoted.

The trusted PVT callback clock is anchored at resync completion. If PVT service
again disappears for 5 s, recovery is re-entered instead of accepting a delayed
first callback.

## STARTING drain hardening

The acquisition STARTING drain now uses the same queue-empty proof. A successful
`checkUblox()` alone is not enough to enter ACQUIRING if bytes remain queued.
This closes the same class of partial/back-to-back output ambiguity at an
acquisition boundary.

## Regression coverage

`test_r3.cpp` models the exact independent-review counterexample:

- an 8 s service gap;
- old PVT/DOP 2000 parsed before a failed read;
- resync entry without callback-clock renewal;
- 100 ms later old PVT/DOP 3000 from a second failed partial read;
- a false/zero-data-like result which is not allowed to finish recovery;
- a successful forced drain with an empty receiver queue;
- post-resync PVT/DOP boundary rejection;
- acceptance only on the following live epoch.

The tests also retain the complete-batch mismatch case, the exact 5000 ms gap
case, normal 1 Hz behavior, session boundaries, UTC snapshot consistency and
all prior R1/R2/R3 regressions. The host `Wire` stub models the bytes-available
register transaction; the SparkFun stub tracks whether `getTimeOfWeek(0)` read a
fresh parsed cache value.

## Remaining physical validation

Real RAK12500/ZOE-M8Q testing still needs deliberate service delays and injected
I2C faults. R4 remains responsible for bounded I2C operations, bus recovery,
hardware-watchdog policy and shared WB_IO2 power ownership. R3.2 only makes the
fresh-position admission rule fail closed while receiver backlog is uncertain.
