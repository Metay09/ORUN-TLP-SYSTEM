# DEVQ1 — Host quality gates: coverage, fuzzing and CodeQL

Status: **PASS — PR #64 merged as `d28beff1c5783988f493ce399c803142004ab968`; residual LOW tooling hardening is tracked in Issue #65.**

Baseline:
`main@9e8e2b8e2d333fbedadb69ed72f316a775579b27`

Branch:
`tooling/host-quality-gates`

## 1. Purpose

DEVQ1 adds developer-side defect-discovery tooling only. It must not change the
production RAK4630 image, framework patches, RF behavior, GNSS behavior,
persistence ownership, BLE runtime, TLP v1 bytes or power policy.

The layers answer different questions:

- normal host suite + ASan/UBSan: do known regression/integration paths remain
  correct, and do sanitizer findings fail the run?
- gcovr: which production source/header paths are exercised?
- libFuzzer + ASan/UBSan: how do selected codecs/persistence parsers behave
  under malformed and semantics-preserving mutated inputs?
- CodeQL: which source-level C/C++ security/correctness/maintainability patterns
  deserve review beyond dynamic tests?

No coverage percentage is a merge threshold.

## 2. Production boundary

Expected and audited production effects:

```text
firmware/src/ changes:            none
firmware/include/ changes:        none
PlatformIO production flags:      unchanged
RAK4630 runtime behavior:         unchanged
RAM / flash:                      0 B intended delta
RF airtime / duty cycle:          unchanged
GNSS acquisition / power:        unchanged
storage layout / wear:            unchanged
BLE UUID / framing / runtime:     unchanged
TLP v1 bytes:                     unchanged
framework patch inputs:           unchanged
```

The independent audit reproduced the pre-fix production build at:

- RAM: 28,976 / 248,832 B (11.6%)
- Flash: 264,912 / 815,104 B (32.5%)

Those values exactly match the M7P7I baseline. A fresh post-fix build is still
required because tooling files changed after that measurement.

## 3. Host sanitizer gate

`firmware/tests/run_host_tests.sh` remains the canonical regression suite.

Sanitizers are default-on. `ORUN_HOST_SANITIZERS` accepts only exact `0` or
`1`. The default sanitizer flags include:

```text
-fsanitize=address,undefined
-fno-sanitize-recover=undefined
```

This closes the independent-audit finding that UBSan could previously print a
runtime error while the process still exited zero.

The only intentional sanitizer-off execution is the CodeQL traced build, where
CodeQL's preload tracer conflicts with ASan's preload ordering. That override is
explicit, logged and source-contract guarded.

## 4. Coverage

`firmware/tests/run_coverage.sh` reruns the complete host suite with GCC
coverage instrumentation and generates a gcovr HTML report.

Post-audit scope includes both:

```text
firmware/src/
firmware/include/
```

This includes header-only production logic that the first DEVQ1 report omitted.

The pre-audit report, limited to `firmware/src/`, was independently reproduced
at:

```text
lines:     92.5% (6052 / 6541)
functions: 98.9% (539 / 545)
branches:  67.1% (4002 / 5966)
```

These are historical pre-fix numbers only. They must not be used as the final
DEVQ1 coverage totals after the header filter change.

No `--fail-under` or equivalent percentage gate exists.

## 5. Fuzzing

`firmware/tests/fuzz/run_fuzz.sh` builds host-only Clang/libFuzzer targets with
ASan + UBSan and fatal undefined-behavior recovery disabled.

Initial targets remain:

1. `tlp_position`
2. `config_format`
3. `security_format`
4. `geofence_format`

The independent audit correctly found that the first persistence harnesses
mostly rejected inputs at CRC before reaching semantic decode. Post-audit
harnesses therefore use two distinct paths:

- raw malformed bytes for rejection/classifier behavior;
- canonical accepted records followed by body mutation plus CRC/commit reseal,
  so semantic validation is actually reached.

Additional corrections:

- config covers legacy v1 and tokenized v2 accepted paths;
- security covers v1/v2 page headers, credentials, legacy TX reserve and v2
  security state;
- security explicitly exercises `decodePageHeaderVersion(..., kVersionV1,...)`;
- geofence has a fixed 564-byte seed from the first corpus load;
- geofence mutation selectors span the full record, including offsets above
  255;
- semantic and physical/torn mutation paths are separate;
- corpus and failure artifacts persist across runs;
- UBSan is fatal via `-fno-sanitize-recover=undefined`.

The pre-audit 10k x4 run completed successfully, but the independent audit showed
that its semantic reach was too shallow. Therefore that old PASS is retained
only as historical evidence; the hardened fuzz targets require a fresh run.

Still outside DEVQ1 fuzz scope are BLE fragment reassembly/logical transport,
`tlp_relay_forward_packet`, `tlp_test_packet`, USB application command parsing
and broader application-request input surfaces. BLE transport is the highest
value follow-up fuzz target before/alongside DEVQ2 physical BLE automation.

## 6. Local CodeQL

The hosted GitHub Actions workflow was removed after repeated jobs failed before
runner dispatch despite repository Actions permissions allowing all actions.
No runner, step or job log was produced, so hosted execution is not a DEVQ1
gate.

Canonical CodeQL execution is local on the owner Debian host.

The setup/runner now require:

- CodeQL CLI exactly 2.27.1;
- official GitHub Linux x64 bundle;
- pinned SHA-256
  `1d380f79896ededc654c7b21fafb3360136f1aeb678ad4df4df9af3910c6b815`;
- a verification marker written only after checksum, version, language and
  query-pack validation;
- exact runtime version check using `version --format=terse`;
- no `CODEQL_BIN` bypass in the canonical runner;
- safe output/install paths under repository `/build/`.

Database extraction traces:

```text
env ORUN_HOST_SANITIZERS=0 ./firmware/tests/run_host_tests.sh
```

Analysis uses:

```text
codeql/cpp-queries:codeql-suites/cpp-security-and-quality.qls
```

The independently reproduced run reported:

```text
193 / 223 C/C++ files extracted/scanned in this invocation
31 SARIF findings
1 error / 19 warning / 11 note
```

The first summarizer incorrectly reported all 31 as unlevelled because it ignored
the SARIF rule's `defaultConfiguration.level`. That is fixed and covered by a
synthetic behavioral regression test.

Detailed finding disposition:
`docs/audits/DEVQ1_CODEQL_DISPOSITION.md`

Independent audit/fix disposition:
`docs/audits/DEVQ1_INDEPENDENT_AUDIT_DISPOSITION.md`

## 7. CodeQL evidence boundary

The host CodeQL database is not exhaustive whole-production analysis.

Known production-relevant gaps include at least:

- `config_incarnation_source.cpp`
- `geofence_incarnation_source.cpp`
- `monotonic_time.cpp`
- `rakwireless/variants/rak4630/variant.cpp`
- framework patch implementation paths
- target-only `NRF52_SERIES` branches not selected by host composition

`main.cpp` is analyzed through the startup host harness and host stubs, not the
exact ARM/SoftDevice target compilation environment.

The 31 CodeQL findings include 11 production-source and 20 host
test/fixture/stub findings. Independent review found zero confirmed production
security/correctness defects. Two callback-singleton lifetime findings remain
important architectural constraints: production `GnssManager` and
`RadioManager` have firmware/static lifetime; a future shorter-lived owner
would require explicit unregister/lifetime handling.

## 8. Destructive-path safety

All user-controlled destructive tooling output paths are now canonicalized and
must be strict children of repository `/build/`.

This applies to:

- explicit host-test output;
- coverage output;
- fuzz output;
- CodeQL installation;
- CodeQL analysis output.

Repository root, `.`, `/build` itself and traversal outside `/build/` are
rejected before destructive cleanup.

`.gitignore` uses root-scoped `/build/`.

## 9. Validation evidence and remaining gates

Pre-audit evidence independently reproduced by the reviewer:

```text
full host suite:                      PASS
pre-fix fuzz 10k x4 runner:           completed PASS, semantic depth inadequate
pre-fix src-only coverage:            92.5 / 98.9 / 67.1
RAK4630 build:                        PASS
RAM / Flash:                          28,976 B / 264,912 B
production delta vs baseline:         0 B / 0 B
CodeQL database/query execution:      PASS
CodeQL extraction:                    193 / 223 C/C++ files
CodeQL findings:                      31
independent audit verdict:            PASS WITH FIXES
dedicated physical DEVQ1 test needed: no
```

Post-audit fixes have been applied.

Post-fix default full host suite is **PASS** on the owner Debian host, including:

- all existing legacy / RF / GNSS / storage / geofence / BLE / startup regressions;
- warnings-as-errors;
- ASan/UBSan with undefined-behavior recovery disabled;
- DEVQ1 tooling source-contract guards;
- DEVQ1 CodeQL SARIF severity regression checks.

No hidden runtime error was reported in the supplied run.

Post-fix hardened fuzz smoke is also **PASS**:

- `tlp_position`: 10,000 executions, PASS;
- `config_format`: 10,000 executions, PASS;
- `security_format`: 10,000 executions, PASS;
- `geofence_format`: 10,000 executions, PASS;
- no ASan/UBSan crash or fatal undefined-behavior finding was reported;
- geofence corpus included a 564-byte full-record seed and reached a
  564-byte corpus input during the run;
- persistent corpus/artifact paths were retained under `build/fuzz/`.

This validates the post-audit H1/H2/L3/L5 tooling changes at bounded-smoke
depth. It is still not exhaustive parser proof.

Post-fix coverage generation is **PASS** with both production source and
header filters enabled:

```text
lines:     92.9% (6563 / 7065)
functions: 98.2% (590 / 601)
branches:  64.8% (5269 / 8132)
```

The denominator increase is expected because `firmware/include/` header-only
production logic is now included. The resulting percentages replace the old
src-only numbers for final DEVQ1 evidence. They remain diagnostic only; no
coverage threshold is a merge gate.

Post-audit verified CodeQL setup is **PASS**:

- official CodeQL 2.27.1 bundle downloaded again because the hardened install
  policy requires a verification marker created by the new setup script;
- pinned SHA-256 verification passed;
- exact CLI version 2.27.1 verified;
- language/query-pack checks passed;
- verified install marker created successfully;
- final setup result: `ORUN local CodeQL setup: PASS`.

Post-fix local CodeQL analysis is **PASS**:

- verified pinned CodeQL CLI 2.27.1 executed successfully;
- database creation traced the canonical host build with the intentional
  `ORUN_HOST_SANITIZERS=0` preload workaround;
- the traced host suite completed successfully;
- all 183 `cpp-security-and-quality` queries completed;
- CodeQL again reported **193 / 223 C/C++ files** in this host invocation;
- corrected SARIF severity resolution produced exactly:
  - **1 error**
  - **19 warnings**
  - **11 notes**
  - **0 unlevelled**
- the 31 findings match the independently reviewed disposition; no new
  production-source finding appeared;
- final runner result: `ORUN local CodeQL execution: PASS`.

Post-fix local CodeQL analysis is **PASS**:

- verified pinned CodeQL CLI **2.27.1** was accepted by the hardened runner;
- database creation traced the canonical host build with the explicit
  `ORUN_HOST_SANITIZERS=0` preload-conflict override;
- the complete traced host suite passed;
- all **183** `cpp-security-and-quality` queries completed;
- CodeQL again reported **193 / 223 C/C++ files** in this invocation;
- SARIF findings remained **31 total**;
- corrected severity resolution produced exactly **1 error / 19 warning / 11 note / 0 none**;
- the one error-severity finding remains the reviewed R2 host-fixture
  `cpp/missing-return`, not a production-source defect.

This validates the post-audit H3/M1 corrections and exactly matches the
independent reviewer's expected severity distribution.

Post-fix RAK4630 production build is **PASS**:

```text
RAM:   28,976 / 248,832 B (11.6%)
Flash: 264,912 / 815,104 B (32.5%)
Build: SUCCESS
```

These values exactly match the M7P7I baseline and the pre-audit DEVQ1 build:
**0 B RAM delta / 0 B Flash delta**. The production source/configuration graph
remains unchanged by DEVQ1.

Focused independent re-review returned **PASS** on the final PR #64 head
`7d7e5326ff342bf89337fe78cfd4bc57cd0f9785`. PR #64 was then squash-merged
to `main` as `d28beff1c5783988f493ce399c803142004ab968`.

No physical-device test is required for DEVQ1 because the production firmware
graph was unchanged.

## 10. Follow-up

DEVQ2 remains the Python/Bleak physical BLE regression harness for real-device
connect / indication / fragmentation / disconnect / reconnect testing.

A small DEVQ1.1 fuzz extension should add the BLE logical transport parser
before or alongside DEVQ2; relay/test packet and USB/application request parsers
can follow based on attack surface and change rate.


## 11. Residual LOW hardening — Issue #65

Issue #65 is a tooling-only follow-up to the final PASS verdict. Active branch:

```text
fix/devq1-tooling-hardening
```

The follow-up closes the three non-blocking residual findings without changing
production firmware:

1. security fuzz CRC/commit offsets are now derived from serialized field sizes
   and protected by compile-time record-layout assertions;
2. CodeQL setup retains the official checksum-verified 2.27.1 bundle archive,
   and both setup and run verify the installed executable byte content against
   the executable inside that SHA-256-pinned archive before trusting it;
3. SARIF severity resolution now applies the SARIF 2.1.0 `kind` rule and
   resolves extension rules through `result.rule.toolComponent.index` plus
   `result.rule.index`.

Focused regression coverage includes a negative executable-tamper test and
synthetic SARIF fixtures for explicit severity, rule defaults, SARIF warning
default, extension-component lookup and non-`fail` results.

Initial isolated validation on the implementation branch:

```text
CodeQL bundle integrity fixture:       PASS
SARIF severity/component fixture:      PASS
security-format libFuzzer smoke:       PASS (2,000 runs, ASan/UBSan)
production src/include changes:        none
```

Before Issue #65 can close, the owner-host full suite, canonical security fuzz
smoke, real pinned CodeQL setup/run and RAK4630 build-delta confirmation remain
merge evidence. A physical-device test is not required unless the production
firmware graph changes.
