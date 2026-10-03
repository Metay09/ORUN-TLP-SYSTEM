#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]

host = (root / "firmware/tests/run_host_tests.sh").read_text(encoding="utf-8")
coverage = (root / "firmware/tests/run_coverage.sh").read_text(encoding="utf-8")
fuzz = (root / "firmware/tests/fuzz/run_fuzz.sh").read_text(encoding="utf-8")
platformio = (root / "firmware/platformio.ini").read_text(encoding="utf-8")
codeql = (root / ".github/workflows/codeql.yml").read_text(encoding="utf-8")

# Coverage is opt-in and reuses the normal host suite; it must not become a
# hidden production/compiler flag or a percentage gate.
assert 'ORUN_HOST_COVERAGE:-0' in host
assert 'coverage_flags=(--coverage -fprofile-abs-path)' in host
assert 'ORUN_HOST_COVERAGE=1' in coverage
assert 'run_host_tests.sh' in coverage
assert '--html-details' in coverage
assert '--fail-under' not in coverage

# Initial fuzzing is host-only, bounded, sanitizer-backed, and covers the
# protocol/persistence parsers selected for this slice.
assert '-fsanitize=fuzzer,address,undefined' in fuzz
for target in (
    'tlp_position',
    'config_format',
    'security_format',
    'geofence_format',
):
    assert f'build_target {target}' in fuzz
assert 'ORUN_FUZZ_RUNS:-10000' in fuzz

# CodeQL observes the host source graph; it does not replace host tests or
# inject flags into the production PlatformIO environment.
assert 'languages: c-cpp' in codeql
assert 'build-mode: manual' in codeql
assert './firmware/tests/run_host_tests.sh' in codeql
assert 'github/codeql-action/init@v4' in codeql
assert 'github/codeql-action/analyze@v4' in codeql

for forbidden in (
    '-fsanitize=fuzzer',
    'ORUN_HOST_COVERAGE',
    'ORUN_FUZZ_RUNS',
):
    assert forbidden not in platformio, forbidden

print("DEVQ1 host quality tooling source-contract guards: PASS")
