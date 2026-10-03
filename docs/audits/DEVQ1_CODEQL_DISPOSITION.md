# DEVQ1 — CodeQL 2.27.1 result disposition

Baseline under review:
`main@9e8e2b8e2d333fbedadb69ed72f316a775579b27`

DEVQ1 branch:
`tooling/host-quality-gates`

Analysis:
- CodeQL CLI 2.27.1
- C/C++ database created from the canonical host build graph
- query suite: `cpp-security-and-quality.qls`
- extracted/scanned in this invocation: 193 / 223 C/C++ files reported by CodeQL
- SARIF findings: 31 total
- SARIF levels: 0 error, 0 warning, 0 note, 31 unlevelled (`none`)

This document records a focused engineering disposition. It does not redefine
CodeQL severity, suppress queries, or weaken compatibility/runtime tests.

## 1. Production-source findings

Eleven findings point at production sources.

### 1.1 GNSS detecting switch length

`cpp/long-switch` at `firmware/src/gnss_manager.cpp:128`.

Disposition: maintainability advisory only. The state-machine case is bounded
and already regression-tested. DEVQ1 must not split or rewrite the proven GNSS
state machine merely to satisfy a style query.

### 1.2 BLE fragment payload comparisons (2)

`cpp/constant-comparison` at `ble_application_transport.cpp:142` and
`:184`.

Disposition: intentional defensive redundancy. Before either check,
`frame_len > kMaxFrameSize` is rejected; therefore
`fragment_payload_len = frame_len - kHeaderSize` cannot exceed
`kMaxPayloadPerFrame`. The second term in each guard still independently
checks declared logical-length bounds. Removing the redundant term would not
improve runtime safety and is not justified in this tooling slice.

### 1.3 I2C endTransmission side effect

`cpp/useless-expression` at `gnss_manager.cpp:41`.

Disposition: model/stub false positive. `Wire.endTransmission(true)` is the
actual I2C transaction termination in the Arduino Wire implementation. It is
called deliberately on the error path after a failed register-byte write and
must not be removed.

### 1.4 Bluefruit advertising mutation calls (2)

`cpp/useless-expression` at `main.cpp:1835-1836` for
`Advertising.addFlags()` and `Advertising.addName()`.

Disposition: host-model false positive. These mutate the real Bluefruit
advertising payload. They are deliberately present because `setName()` alone
does not advertise the name; this behavior was already physically exercised in
the BLE qualification path. Do not remove them to silence host-only analysis.

### 1.5 Long application response functions (3)

`cpp/poorly-documented-function` at:
- `ble_application_transport.cpp:290`
- `security_store.cpp:1029`
- `usb_application_adapter.cpp:46`

Disposition: maintainability/style advisories. No defect is implied by the
query. Splitting proven state-machine/serialization code only to reduce function
length is outside DEVQ1 and would increase regression surface.

### 1.6 Callback singleton lifetime (2)

`cpp/stack-address-escape` at:
- `gnss_manager.cpp:62`
- `radio_manager.cpp:151`

Disposition: valid architectural lifetime warning, but no current production
defect identified. Both modules publish `this` to a file-scope callback bridge.
The production composition owns `gnss_manager` and `radio_manager` as
namespace-scope objects in `main.cpp`, so their lifetime is the firmware
lifetime. Existing callback/ownership tests exercise the handoff behavior.

Constraint recorded: a future refactor must not instantiate either callback
owner with a shorter lifetime unless an explicit unregister/destruction
protocol is added. DEVQ1 does not alter this proven callback architecture.

## 2. Test/harness-source findings

Twenty findings point only at host tests, fixtures or stubs:

- 6 unused static fixture vectors in
  `m7p6e_candidate_contract.h`;
- 1 missing-return report on the R2 test entry point;
- 3 deliberate `.cpp` includes used by host composition/source-contract
  harnesses;
- 1 non-constant-format report in the Arduino host stub;
- 2 constant-comparison reports in test code;
- 1 long/poorly-documented test function;
- 6 stack-address-escape reports in M4 host fixtures.

Disposition: no production runtime impact identified. These are harness
construction/style/model findings. They are not grounds to weaken or rewrite
fixtures that protect compatibility, RF ownership, startup composition or
storage behavior.

The R2 source-level entry point is ordinary C++ `main()`; falling off the end
of `main` is defined as returning zero. No production code change is needed.

## 3. Security conclusion for DEVQ1

Focused review found:

- confirmed production memory-corruption/security defect: **0**
- confirmed production correctness defect: **0**
- production lifetime invariant worth preserving: **2 callback-owner findings**
- maintainability/defensive/model advisories: remaining production findings
- test/harness-only findings: **20**

No CodeQL finding justifies changing TLP v1 bytes, RF behavior, GNSS acquisition,
storage format, BLE framing, power policy, framework patches or proven state
machines.

The analysis is useful precisely because it surfaced the callback lifetime
constraint and distinguished real platform side effects from host-stub
modeling. The result should be carried into the final independent audit rather
than treated as an automatic clean bill of health.

## 4. Evidence boundary

CodeQL reported scanning 193 / 223 C/C++ files in this host-build invocation.
Therefore this result is **not** described as exhaustive whole-repository or
whole-framework coverage. The production PlatformIO build remains a separate
compiler/build gate, already PASS with exact 0 B DEVQ1 RAM/Flash delta.

No physical-device claim is made from CodeQL.
