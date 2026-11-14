# KQ-04 — Device-wide 869.525 MHz radio duty guard (PC gate)

Status: WIRED TO ALL RADIO TX ENTRYPOINTS ON DRAFT PR #305 / LOCAL PC VERIFICATION PENDING / NOT FIELD-READY.

## Source and limits

- Target RF profile: 869.525 MHz, SF11/BW125. Turkish BTK general SRD band
  869.400–869.650 MHz permits duty cycle at or below 10% (subject to
  equipment power and all other applicable constraints); this is an upper
  limit, not a required operating target.
- A **device-wide** hourly budget of 360,000 ms is the reference maximum.
  Every on-air transmission from the device must be charged, including
  POSITION, HISTORY_SECURE, PRODUCT_SECURE, RELAY, CUSTODY_ACK, LoRa commands/
  results, and test traffic.
- Existing firmware's Sf5l0DrainPacer Config::duty_ppm=60000 is a **6%
  PRODUCT_SECURE-only** limit and does not represent aggregate RF safety.
  DO NOT raise it to 100000 in isolation.

## Regression / PC model

Run on your own PC (no GitHub Actions, device, live service, or DB required):

    python3 firmware/tests/sf5/test_kq04_rf_budget_host.py -v

The Python reference model checks aggregate frame admission, rolling hour,
retries, ACKs, unknown TX classes, and 24/48-hour flash retention pressure.
It deliberately does not modify the firmware; PASS is a **test harness PASS**,
not evidence of on-device enforcement or legal conformity.

## Native C++ gate and live RadioManager integration (draft)

The draft now includes `firmware/include/rf_duty_budget.h`,
`firmware/src/rf_duty_budget.cpp` and native host tests. The per-device
ledger uses 61 conservative minute buckets and counts seven packet classes.
All seven actual `Radio.Send` call sites in `radio_manager.cpp` and
`radio_manager_custody_ack.cpp` are now preceded by an airtime reservation.
LoRa airtime is estimated from the actual configured SF/BW/CR/preamble/CRC
and frame length (ceil in milliseconds). The gate is shared across role
changes and Radio.Sleep() wakeups, but each real `RadioManager::begin()`
starts a fresh budget. The old 6% PRODUCT_SECURE pacer remains a distinct
additional throughput limiter, not a substitute for this total.

**Permanent ORUN owner decision:** a real cold/warm boot starts with full
budget immediately. No prior-boot carryover or enforced start-up hold.
Repeated restarts can exceed a continuous rolling-hour RF constraint;
neither a PC test nor this implementation proves regulatory conformity.

Run the isolated native C++ test on the PC:

    g++ -std=c++11 -Wall -Wextra -Werror -pedantic \
      -fsanitize=address,undefined -fno-sanitize-recover=undefined \
      -Ifirmware/include \
      firmware/tests/sf5/test_kq04_rf_duty_budget.cpp \
      firmware/src/rf_duty_budget.cpp -o /tmp/orun-kq04-rf-test
    /tmp/orun-kq04-rf-test

The native test checks the airtime estimator and ledger. Additionally, the
normal PC host suite now compiles `test_sf5j_radio_transport.cpp` against
the real RadioManager and fake SX126x Radio.Send; it verifies an exhausted
budget physically blocks the mock Send and does not reset on role transitions.
The bridge paths are **not yet operator-tested** on the current branch.
The new limiter can delay one-minute live reports, backlog sends, commands,
or ACKs when the aggregate quota is exhausted; no scheduler-level priority
reservation or guaranteed latest-position deadline is implemented here.

## Remaining acceptance gates before merge or physical rollout

1. **Implemented in draft:** one RadioManager-owned budget for every TX
   class and a Semtech-equation ceil(ms) airtime estimate. Attempts reserve
   BEFORE Radio.Send (and retain the reservation on timeouts).
2. **Implemented in draft:** relay uses a non-consuming due check before
   airtime admission; Gateway preflights the 56B ACK before consuming its
   one-shot authenticated ACK. Independently test these denial paths.
3. Always reset the budget to full on boot, with immediate TX eligibility;
   ignore previous power cycles as an explicit design decision. Test normal
   millis() rollover, time jumps, sleep/wake, role changes and TX errors.
   Do not claim this reset behavior enforces a continuous-hour legal maximum
   across reboot boundaries.
4. Preserve custody semantics: a delayed Gateway ACK is not proof of delivery,
   and no Tracker record may be released without verified exact-object ACK.
   A duty denial must leave the record durable and eligible for a later retry.
5. Preserve priority without starvation: recent position and mandatory ACK
   reservations outrank backlog, but backlogged data must have a documented
   bounded opportunity when capacity exists. Duty budget may never be
   borrowed above the legal maximum to satisfy an ACK.
6. **Partially implemented; PC output pending:** native C++ host tests and
   real RadioManager fake-Send exhaustion regression. Run full host suite
   and `pio run -e rak4630` in a fresh, safe local merge rehearsal.
   Add missing denial and 24/48h outage coverage before field enablement.
7. Measure real SX1262 on-air duration, RF power/ERP and antenna setup before
   a field-release claim. Regulatory compliance is not established by source
   inspection or the Python reference test alone.

## Observed KQ-04 evidence (7 Nov 2026)

- Tracker 52E16773141A8887 / incarnation 5523010CEA37319F:
  backend held 7,147 of sequence 1..7240, missing 93 sequence numbers in
  ten gaps concentrated in 2855..3107.
- Tracker status: retained=0, lost=96 (cumulative flash-capacity losses),
  BENCH DRAIN sent=322/released=322/ack_timeouts=0 (live measurement).
- The observed missing period was around 7 Nov 00:51–05:02 local; some
  neighboring records reached PC Edge roughly 20–22 h later, then typically
  reached backend within seconds. This supports backlog/retention pressure,
  but does not prove a particular missing record was lost at a particular
  flash rotation.

## No automatic firmware upload / no current pilot activation

The unchanged 6% pacer default remains. This is source only on a DRAFT PR;
no physical firmware was flashed, no live service was restarted, and no
production data was mutated. Do not merge this work as qualified until PC
test evidence, RF capacity/scheduler review and physical tests are complete.
