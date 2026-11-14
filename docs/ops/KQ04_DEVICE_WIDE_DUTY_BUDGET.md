# KQ-04 — Device-wide 869.525 MHz radio duty guard (PC gate)

Status: REFERENCE TEST ONLY / NOT WIRED INTO RADIO / NOT FIELD-READY.

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

## Native C++ groundwork (not connected to RadioManager)

The draft now also includes `firmware/include/rf_duty_budget.h` and
`firmware/src/rf_duty_budget.cpp` plus a pure host test. This is a
conservative 61 x 1-minute bucket accounting component with seven explicitly
registered packet classes. On every fresh boot it refuses ALL TX for 60 minutes,
because the previous hour's airtime has no durable authority. The source
is compiled but **never called** by the live radio paths. The one-hour
boot hold is a safety fallback, **not an acceptable user experience** for a
collar that must report soon after restarting.

Run the isolated native C++ test on the PC:

    g++ -std=c++11 -Wall -Wextra -Werror -pedantic \
      -fsanitize=address,undefined -fno-sanitize-recover=undefined \
      -Ifirmware/include \
      firmware/tests/sf5/test_kq04_rf_duty_budget.cpp \
      firmware/src/rf_duty_budget.cpp -o /tmp/orun-kq04-rf-test
    /tmp/orun-kq04-rf-test

This verifies the native component in isolation only. The next firmware PR
must establish a boot-safe persisted budget or acceptable documented cold-start
policy, plus real SX126x time-on-air and complete send-path routing **before**
enabling this gate in RadioManager. Do not wire this component directly to live
TX without resolving that startup behavior.

## Production implementation gates (separate firmware PR)

1. Enumerate all SX126x Radio.Send call sites, including indirect Gateway
   custody ACK and relay paths. Route them through a single owner that
   reserves estimated air-time before entering TX. No bypass is permitted.
2. Compute actual frame air-time from modulation config and payload length
   (including preamble/headers). Count attempted transmissions conservatively,
   including TX timeout, retransmission and interrupted TX.
3. Ensure the aggregate budget cannot be bypassed by reboot/uptime rollover,
   time jumps, sleep/wake, role changes or TX errors. A RAM-only sliding hour
   must NOT be described as reboot-safe. Cold-start behavior must fail safely
   until a durable accounting or conservative cool-down mechanism exists.
4. Preserve custody semantics: a delayed Gateway ACK is not proof of delivery,
   and no Tracker record may be released without verified exact-object ACK.
   A duty denial must leave the record durable and eligible for a later retry.
5. Preserve priority without starvation: recent position and mandatory ACK
   reservations outrank backlog, but backlogged data must have a documented
   bounded opportunity when capacity exists. Duty budget may never be
   borrowed above the legal maximum to satisfy an ACK.
6. Add native C++ host regression with fake clock and fake Radio.Send,
   including rolling 1h, reboot, SF11 airtime, packet classes, high backlog,
   and 24/48-hour outage. Run the firmware build locally on PC.
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

## Non-goals of this first PR

No change to 6% pacer default; no firmware release or flash; no service
restart; no alteration or deletion of production data. Do not merge this
reference-only PR as if it solved KQ-04 or issue #221.
