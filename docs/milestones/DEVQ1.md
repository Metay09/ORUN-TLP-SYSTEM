# DEVQ1 — Host quality gates: coverage, fuzzing and CodeQL

Status: **IMPLEMENTATION CANDIDATE — host revalidation PASS; tooling validation continues.**

Baseline:
`main@9e8e2b8e2d333fbedadb69ed72f316a775579b27`

Branch:
`tooling/host-quality-gates`

## 1. Purpose

This slice adds developer-side evidence only. It must improve defect discovery
without changing the production RAK4630 image, framework patches, RF behavior,
GNSS behavior, persistence ownership, BLE runtime, protocol bytes or power
policy.

The three added layers answer different questions:

- host coverage: which production source lines/branches are actually exercised
  by the existing host suite?
- libFuzzer + ASan/UBSan: what happens when protocol/persistence parsers receive
  malformed, truncated or mutated bytes?
- CodeQL: are there source-level C/C++ security/memory/data-flow patterns that
  the dynamic tests do not expose?

No coverage percentage is a merge threshold in DEVQ1. Coverage is diagnostic
evidence for finding blind spots, not a score to game.

## 2. Production boundary

DEVQ1 must remain outside the production firmware graph.

Expected production effects:

```text
RAK4630 source/runtime behavior: unchanged
PlatformIO production flags:     unchanged
RAM / flash:                     0 B intended delta
RF airtime / duty cycle:         unchanged
GNSS acquisition/power:          unchanged
storage layout/wear:             unchanged
BLE UUID/framing/runtime:        unchanged
TLP v1 bytes:                    unchanged
framework patches:               unchanged
```

The normal `run_host_tests.sh` receives only two optional environment-controlled
test features:

- `ORUN_HOST_TEST_DIR` selects a persistent host object/output directory;
- `ORUN_HOST_COVERAGE=1` adds GCC coverage instrumentation.

The default path remains the existing ASan/UBSan warnings-as-errors suite.

## 3. Coverage

`firmware/tests/run_coverage.sh` reruns the complete existing host suite with
GCC coverage instrumentation, then asks `gcovr` for an annotated HTML report
limited to `firmware/src/`.

DEVQ1 deliberately does not use `--fail-under` or any equivalent threshold.
The first useful output is a gap inventory, especially for recovery, timeout,
wraparound, malformed-input and fail-closed branches.

Coverage files are written under `build/host-coverage/` and are ignored by
Git.

## 4. Fuzzing

`firmware/tests/fuzz/run_fuzz.sh` builds host-only Clang/libFuzzer targets with
AddressSanitizer and UndefinedBehaviorSanitizer.

Initial bounded targets:

1. `tlp_position`
   - arbitrary POSITION bytes;
   - valid decode -> canonical re-encode equivalence.

2. `config_format`
   - legacy v1 and tokenized v2 decode/classification;
   - near-valid v2 mutation around a canonical encoded record.

3. `security_format`
   - page header, credential, legacy TX reserve and v2 security-state records;
   - near-valid typed state mutation.

4. `geofence_format`
   - full durable record decode/classification;
   - canonical CLEAR/configured records followed by bounded mutation.

The default smoke budget is 10,000 executions per target and can be changed with
`ORUN_FUZZ_RUNS`. A finding must preserve the generated artifact as a regression
fixture before the bug is considered closed.

Fuzzing is host evidence only. It does not prove flash power-cut behavior,
SoftDevice concurrency, RF/GNSS behavior or hardware correctness.

## 5. CodeQL

`.github/workflows/codeql.yml` adds C/C++ CodeQL analysis for firmware changes.
The workflow uses manual build mode and builds the same host source graph through
`firmware/tests/run_host_tests.sh`.

This intentionally avoids changing PlatformIO or adding CodeQL instrumentation
to the production image.

The first workflow uses the extended security/quality query suites. Findings are
review inputs, not automatic permission to rewrite proven state machines or
compatibility fixtures.

## 6. Source contract

`firmware/tests/tooling/test_tooling_contract.py` is part of the normal host
suite and guards the separation:

- coverage remains opt-in;
- no coverage percentage threshold is silently introduced;
- fuzz targets remain host sanitizer binaries;
- CodeQL uses the host source graph;
- no fuzz/coverage environment or sanitizer flags enter `platformio.ini`.

## 7. Validation plan

Because this slice changes test/CI infrastructure but not production runtime:

1. complete existing host suite;
2. tooling source-contract;
3. coverage runner on the owner Debian environment;
4. bounded fuzz smoke on the owner Debian environment;
5. production `pio run -e rak4630` and exact RAM/Flash comparison to
   `main@9e8e2b8` to prove intended 0 B runtime delta;
6. CodeQL workflow result on the PR;
7. focused independent audit of tooling isolation and false-confidence risks.

No dedicated physical device test is required for DEVQ1 because it adds no
device-side behavior.

## 8. Validation evidence so far

Owner-run full host suite on the DEVQ1 branch is **PASS**, including:

- all existing legacy / RF / GNSS / storage / geofence / BLE / startup regressions;
- warnings-as-errors plus the existing ASan/UBSan coverage in the host suite;
- the new `DEVQ1 host quality tooling source-contract guards`.

This confirms the default host-test path remains compatible after adding the
optional coverage plumbing and tooling contract. It does not yet validate the
libFuzzer runner, gcovr report generation, production RAK4630 size invariance or
CodeQL execution.

## 8. Follow-up, not this slice

A Python/Bleak physical BLE regression harness is intentionally separate
(DEVQ2). It will exercise real GATT connect/indicate/fragment/reconnect behavior
and therefore has a different evidence boundary from these host/CI tools.
