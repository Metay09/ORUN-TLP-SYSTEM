# DEVQ1 — Independent audit disposition

Independent audit verdict on pre-fix head `790c097`: **PASS WITH FIXES**.

The audit independently reproduced:
- complete host suite PASS;
- bounded 10k x4 fuzz runner completion;
- coverage 92.5% lines / 98.9% functions / 67.1% branches for the then-current
  `firmware/src/`-only report;
- RAK4630 RAM 28,976 B / Flash 264,912 B;
- exact zero production binary delta versus M7P7I baseline;
- CodeQL 193 / 223 extracted C/C++ files and 31 findings;
- official CodeQL asset checksum.

No production runtime defect was identified. The fixes below are tooling and
evidence-quality corrections only.

## HIGH findings

### H1 — UBSan recovery allowed PASS after runtime UB

Accepted.

Fix:
- normal host sanitizer flags now include
  `-fno-sanitize-recover=undefined`;
- libFuzzer flags include the same;
- `ORUN_HOST_SANITIZERS` now accepts only exact `0` or `1`;
- sanitizer-off execution is logged and reserved for the CodeQL preload trace.

Effect: a UBSan runtime finding now terminates the affected host/fuzz process
instead of relying on manual log review.

### H2 — persistence fuzz targets rarely reached accepted semantic decode

Accepted.

Fix:
- config fuzz now exercises canonical accepted v1/v2 records, then mutates body
  bytes and recomputes CRC/commit so semantic validators are reached;
- security fuzz does the same for v1/v2 page headers, credentials, legacy
  TX-reserve and v2 security-state records;
- security fuzz now explicitly exercises
  `decodePageHeaderVersion(..., kVersionV1, ...)`;
- geofence fuzz now has a 564-byte seed from the first run;
- geofence mutation indexes are derived from a 32-bit selector and span the
  complete 564-byte record, not only offsets 0..255;
- a semantic mutation path recomputes CRC/commit while a separate physical
  mutation path deliberately leaves CRC/commit/torn evidence inconsistent;
- accepted records retain canonical round-trip invariants;
- corpus and crash artifacts persist between runs.

The bounded 10k smoke remains a smoke gate, not exhaustive correctness proof.

### H3 — SARIF severity inheritance was reported incorrectly

Accepted.

Fix:
- summarizer now resolves explicit `result.level`, then the reporting rule's
  `defaultConfiguration.level`, then the SARIF default warning;
- driver and extension rules are both indexed;
- a synthetic behavioral regression test covers error, warning, note,
  extension-rule and explicit override cases;
- CodeQL disposition now records the independently resolved distribution:
  **1 error / 19 warning / 11 note**.

The one error-level result is the R2 host-fixture missing-return finding, not a
production-source finding.

## MEDIUM findings

### M1 — CodeQL runtime pin could be bypassed

Accepted.

Fix:
- canonical version is fixed at 2.27.1;
- default installation requires a verification marker containing version and
  pinned SHA-256;
- missing/invalid marker forces a fresh checksum-verified install;
- setup and run both check `codeql version --format=terse`;
- `CODEQL_BIN` override is rejected by the canonical runner;
- incompatible `ORUN_CODEQL_VERSION` overrides are rejected.

### M2 — important untrusted-input parsers are not yet fuzzed

Accepted as a documented scope gap.

DEVQ1 covers the four initial targets only. Still outside this slice:
- BLE fragment reassembly / logical transport;
- `tlp_relay_forward_packet`;
- `tlp_test_packet`;
- USB application command parsing;
- broader `application_request` input surfaces.

The highest-value next fuzz extension is BLE logical transport, tracked as a
follow-up before/alongside DEVQ2 physical BLE automation. This gap is not hidden
by the current four-target PASS.

### M3 — CodeQL unscanned production sources were not named

Accepted.

The CodeQL disposition now names known gaps including incarnation-source files,
monotonic-time production source, RAK4630 variant code, framework patch paths
and target-only NRF52 branches. Host CodeQL is not described as exact ARM /
SoftDevice whole-production analysis.

### M4 — coverage excluded header-only production logic

Accepted.

Fix:
- gcovr now filters both `firmware/src/` and `firmware/include/`;
- no percentage threshold was added.

The old 92.5 / 98.9 / 67.1 values remain historical pre-fix evidence and must
not be reported as the final post-audit totals. New totals require revalidation.

### M5 — destructive output paths accepted unsafe user values

Accepted.

Fix:
- host-test explicit output, coverage, fuzz, CodeQL install and CodeQL analysis
  paths are resolved and required to be strict children of repository
  `/build/`;
- `.`, repository root, `build` itself and traversal outside `build/`
  are rejected before `rm -rf`;
- `.gitignore` now uses root-scoped `/build/`.

## LOW findings

Addressed where low-risk and useful:
- L1: corrected the R2 missing-return explanation; the earlier “ordinary main”
  rationale was wrong.
- L2: sanitizer override is strict 0/1, sanitizer-off is logged, and the
  source-contract ensures explicit `ORUN_HOST_SANITIZERS=0` occurs only in
  the CodeQL runner.
- L3: fuzz corpus and failure artifacts are no longer deleted at the start of
  each run.
- L4: root build ignore is now `/build/`.
- L5: security fuzz explicitly exercises the supported v1 page-header decoder.

## Production boundary

These fixes do not modify:
- `firmware/src/`;
- `firmware/include/`;
- `firmware/platformio.ini`;
- RF/GNSS/BLE/storage/power runtime behavior;
- TLP v1 bytes;
- framework patch inputs.

A fresh post-fix host/fuzz/coverage/CodeQL/build validation is required before
merge. No dedicated physical-device test is required because the production
firmware graph remains unchanged.
