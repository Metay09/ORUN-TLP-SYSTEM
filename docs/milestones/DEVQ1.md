# DEVQ1 — Host quality gates: coverage, fuzzing and CodeQL

Status: **IMPLEMENTATION CANDIDATE — host + bounded fuzz + coverage + production size invariance + local CodeQL setup PASS; CodeQL analysis/audit remain.**

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

DEVQ1 originally added a GitHub Actions CodeQL workflow. Repository Actions
permissions were confirmed enabled, but repeated PR runs failed before runner
dispatch: no runner was assigned, no step started, and no job log existed.
Because that hosted-infrastructure failure is outside firmware correctness, the
hosted workflow was removed rather than leaving a permanent red check.

The canonical DEVQ1 CodeQL gate is now local CLI execution on the owner Debian
host:

- `firmware/tests/codeql/setup_codeql.sh` downloads the official GitHub CodeQL
  Linux x64 bundle pinned to **2.27.1** and verifies the published SHA-256 before
  extraction;
- `firmware/tests/codeql/run_codeql.sh` creates a C/C++ database by tracing the
  same canonical `firmware/tests/run_host_tests.sh` build graph;
- analysis uses the bundled
  `cpp-security-and-quality.qls` suite, which already includes the
  security-extended queries plus maintainability/reliability queries;
- results are kept as SARIF under `build/codeql/` and summarized to the
  terminal for review.

This intentionally avoids changing PlatformIO or adding CodeQL instrumentation
to the production image. Findings are review inputs, not automatic permission to
rewrite proven state machines or compatibility fixtures.

## 6. Source contract

`firmware/tests/tooling/test_tooling_contract.py` is part of the normal host
suite and guards the separation:

- coverage remains opt-in;
- no coverage percentage threshold is silently introduced;
- fuzz targets remain host sanitizer binaries;
- local CodeQL uses the host source graph and a checksum-pinned official bundle;
- the failing hosted CodeQL workflow is absent;
- no fuzz/coverage/CodeQL environment or sanitizer flags enter `platformio.ini`.

## 7. Validation plan

Because this slice changes test/CI infrastructure but not production runtime:

1. complete existing host suite;
2. tooling source-contract;
3. coverage runner on the owner Debian environment;
4. bounded fuzz smoke on the owner Debian environment;
5. production `pio run -e rak4630` and exact RAM/Flash comparison to
   `main@9e8e2b8` to prove intended 0 B runtime delta;
6. pinned local CodeQL database creation + security-and-quality analysis;
7. focused independent audit of tooling isolation and false-confidence risks.

No dedicated physical device test is required for DEVQ1 because it adds no
device-side behavior.

## 8. Validation evidence so far

Owner-run full host suite on the DEVQ1 branch is **PASS**, including:

- all existing legacy / RF / GNSS / storage / geofence / BLE / startup regressions;
- warnings-as-errors plus the existing ASan/UBSan coverage in the host suite;
- the new `DEVQ1 host quality tooling source-contract guards`.

This confirms the default host-test path remains compatible after adding the
optional coverage plumbing and tooling contract.

Owner-run bounded libFuzzer smoke is also **PASS**:

- `tlp_position`: 10,000 executions, no crash / ASan / UBSan finding;
- `config_format`: 10,000 executions, no crash / ASan / UBSan finding;
- `security_format`: 10,000 executions, no crash / ASan / UBSan finding;
- `geofence_format`: 10,000 executions, no crash / ASan / UBSan finding;
- final runner result: `ORUN bounded host fuzz smoke: PASS`.

This is bounded host evidence only; it is not proof of exhaustive parser
correctness or hardware behavior.

Owner-run gcovr 5.2 coverage generation is **PASS** after using the Debian 12
compatible `--print-summary` flag:

- lines: **92.5%** (6052 / 6541);
- functions: **98.9%** (539 / 545);
- branches: **67.1%** (4002 / 5966).

DEVQ1 intentionally does not turn these percentages into merge thresholds.
The branch number is useful as a gap-finder for defensive/error/recovery paths,
not as a score to optimize.

Owner-run production RAK4630 build is **PASS**:

- RAM: **28,976 / 248,832 B (11.6%)**;
- Flash: **264,912 / 815,104 B (32.5%)**;
- build result: `SUCCESS`;
- exact match to the M7P7I baseline: **0 B RAM / 0 B Flash delta**.

This proves the DEVQ1 tooling files and optional host flags do not enter the
production RAK4630 image at the current baseline.

Hosted CodeQL root cause was isolated to pre-runner dispatch behavior rather
than repository code execution: repeated PR jobs had no assigned runner, no
steps and no logs despite repository Actions permissions allowing all actions.
DEVQ1 therefore does not treat the hosted runner as a required gate.

Owner-run local CodeQL setup is **PASS**:

- official Linux x64 CodeQL bundle **2.27.1** downloaded successfully;
- pinned SHA-256 verified before extraction;
- CLI reports `CodeQL command-line toolchain release 2.27.1`;
- bundled C/C++ language/query packs resolved;
- final setup result: `ORUN local CodeQL setup: PASS`.

Remaining validation: local CodeQL database creation + analysis/result review and
focused independent audit.

## 9. Follow-up, not this slice

A Python/Bleak physical BLE regression harness is intentionally separate
(DEVQ2). It will exercise real GATT connect/indicate/fragment/reconnect behavior
and therefore has a different evidence boundary from these host/CI tools.
