# Portable device-ID serial diagnostics

Base: `8f48c7157b3664d1db7c620223429395c0ea7f69`.
Production branch: `fix/serial-log-formatting`.

## Root cause: CONFIRMED

The installed Adafruit core's `Print::printf` calls `vsnprintf`. PlatformIO's
Adafruit build selects `--specs=nano.specs`; the actual pre-fix firmware ELF
contains nano `_svfprintf_r` (aliased to `_svfiprintf_r`) and `_printf_i`.
Inspection of that linked machine code confirms the failure, independently
of host libc behavior:

- At `0x44f4e..0x44f6c`, the format parser looks up one modifier in `hlL`,
  advances the format pointer once, then reads the conversion character.
  There is no second-`l` handling.
- `%016llX` therefore passes the second `l` to the unsupported-conversion
  path. `_printf_i` emits it, padded to width 16, without consuming an argument;
  `X` is then literal text. Integer conversions advance the argument list by
  four bytes, not the eight bytes of the unconsumed device ID.
- On the little-endian target, the following sequence field consumes
  `0x4B275BA5` = `1260870565` (A's low ID word), and RX's RSSI field consumes
  `0x09A462BD` = `161768125` (A's high ID word). The SNR field then consumes
  the actual sequence. This exactly explains the prior physical log.

These addresses refer to the pre-fix beacon ELF inspected before rebuilding;
they are not stable addresses for future builds.

## Fix and scope

All device-ID diagnostics in `radio_manager.cpp` use `%08lX%08lX` with explicit
`uint32_t` truncation and matching `unsigned long` arguments, upper half first.
This retains exactly 16 uppercase hex digits, including leading zeros, on both
the target and a host where `unsigned long` is 64 bits. RSSI/SNR arguments are
explicitly converted to `int` for `%d`.

TEST, POSITION, RELAY and BASE logs, including the second relay ID, are covered.
Wire encoding, sequence allocation, radio settings and network logic are unchanged.
The production branch leaves TEST beacons disabled.

## Regression coverage

The R2 serial stub captures real production output and rejects long-long printf
arguments at compile time. This includes compiled paths not executed by a
particular host scenario (such as disabled TEST TX). The new guard fails against
the original production code.

Runtime checks cover ID zero, upper/lower word boundaries, the physical A ID,
all-one bits, leading zeros, maximum sequence, signed RSSI/SNR, POSITION TX,
relay receive/queue/duplicate/drop/transmit, BASE direct NEW/DUP, BASE relayed
receive with two distinct IDs, and ignored POSITION diagnostics.

Physical verification is performed separately with a temporary beacon branch;
host formatting tests alone do not establish RF behavior.

Local validation: full `firmware/tests/run_host_tests.sh` PASS with
`-Wall -Wextra -Werror`, ASan/UBSan; `pio run` SUCCESS with beacons disabled
(RAM 13,844 / 248,832 bytes, flash 139,544 / 815,104 bytes);
`git diff --check` PASS. No beacon configuration is included in the fix.
