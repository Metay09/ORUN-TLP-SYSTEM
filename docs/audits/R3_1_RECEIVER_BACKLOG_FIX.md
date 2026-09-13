# R3.1 — GNSS receiver/FIFO backlog freshness guard

Base: `f458ac1` on `fix/r3-gnss-freshness`.

## Why R3 needed one more guard

R3 correctly aged PVT and DOP candidates from callback arrival and prevented
same-iTOW pairing from renewing that age. A final independent review found a
transport-level counterexample: during ACQUIRING, if I2C service is delayed,
ZOE-M8Q can accumulate multiple NAV-PVT/NAV-DOP messages. SparkFun 2.2.29
`checkUbloxI2C()` drains the announced byte batch synchronously. While parsing
that batch, `packetUBXNAVPVT->data` is updated for every PVT, but callbackData
keeps the first unconsumed callback copy until `checkCallbacks()` dispatches it.
The old callback therefore receives a new local callback timestamp even though
its receiver measurement may be older than the 5 s freshness limit.

This means callback-arrival age alone cannot prove receiver measurement age.
The R3.1 guard closes that gap without changing the POSITION wire format or
patching the SparkFun dependency.

## Guard 1: callback iTOW versus newest parsed PVT cache

`GnssManager::handlePvt()` reads `gnss.getTimeOfWeek(0)` immediately from the
PVT callback. In the inspected 2.2.29 flow, `processUBXpacket()` marks the PVT
iTOW field fresh whenever a PVT is parsed; no production code reads that getter
between `checkUblox()` and the PVT callback. Therefore the zero-wait getter
returns the newest PVT parsed in the just-drained batch instead of initiating a
new blocking GNSS wait.

If callback `pvt_data.iTOW` differs from that newest parsed iTOW, the callback is
provably not the newest PVT in the batch. R3.1 clears both PVT/DOP candidates,
marks the discarded PVT as a boundary, increments
`receiver_backlog_rejected`, and does not create a fresh candidate.

The mutable cache is used only as a transport/backlog witness. UTC still comes
exclusively from the immutable NAV-PVT callback snapshot; `getUnixEpoch()` and
calendar getters remain absent from the fresh POSITION path.

## Guard 2: long PVT callback silence

A batch can contain only one stale PVT, in which case callback iTOW equals the
current cache and Guard 1 cannot distinguish its age. R3.1 also records the
local time of the previous PVT callback inside the active acquisition. If the
next PVT callback arrives after an interval greater than or equal to
`kFreshFixMaxAgeMs` (5000 ms), that first resumed PVT is conservatively treated
as a resynchronization boundary and rejected.

The callback timer resets for every acquisition. The existing STARTING drain
and first-epoch PVT/DOP boundaries still protect acquisition/session changes.
R3.1 adds only the missing in-session backlog protection.

The trade-off is deliberate: a genuinely fresh PVT received after a >=5 s
callback outage is also discarded once. At the configured 1 Hz navigation
rate, the next epoch can be accepted approximately one second later. This is
preferable to reporting an old receiver-buffered coordinate as fresh.

## Regression coverage

`firmware/tests/r3/test_r3.cpp` adds two deterministic cases using the inspected
SparkFun callback/cache semantics:

1. Multi-PVT batch: callback copy remains on iTOW 2000 while current cache
   advances to iTOW 3000. Matching old DOP is delivered first. The old pair must
   not produce a fix, candidates are cleared, and the next live epoch succeeds.
2. Single stale PVT: callback and current cache have the same iTOW after exactly
   5000 ms of PVT callback silence. The resumed epoch must be rejected and the
   following live epoch must succeed.

The M3 SparkFun stub now exposes `getTimeOfWeek(0)` from the modeled mutable PVT
cache, and normal single-PVT helpers keep callback and current cache aligned.
Existing delayed PVT/DOP, UTC snapshot, session, detection, storage-age and radio
admission regressions remain in the same host suite.

## Remaining validation

This is still software validation. Physical RAK12500/ZOE-M8Q testing should
exercise deliberate main-loop/I2C service delays and confirm that the first
buffered callback is rejected, that fresh 1 Hz operation resumes on the next
epoch, and that no unexpected getter-triggered I2C transaction occurs between
parser completion and callback dispatch. R4 still owns bounded I2C recovery,
hardware watchdog policy and shared WB_IO2 sensor-power ownership.
