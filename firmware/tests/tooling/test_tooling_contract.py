#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]

host = (root / "firmware/tests/run_host_tests.sh").read_text(encoding="utf-8")
coverage = (root / "firmware/tests/run_coverage.sh").read_text(encoding="utf-8")
fuzz = (root / "firmware/tests/fuzz/run_fuzz.sh").read_text(encoding="utf-8")
platformio = (root / "firmware/platformio.ini").read_text(encoding="utf-8")
codeql_setup = (root / "firmware/tests/codeql/setup_codeql.sh").read_text(encoding="utf-8")
codeql_run = (root / "firmware/tests/codeql/run_codeql.sh").read_text(encoding="utf-8")
codeql_summary = (root / "firmware/tests/codeql/summarize_sarif.py").read_text(encoding="utf-8")

# Coverage is opt-in and reuses the normal host suite; it must not become a
# hidden production/compiler flag or a percentage gate.
assert 'ORUN_HOST_COVERAGE:-0' in host
assert 'coverage_flags=(--coverage -fprofile-abs-path)' in host
assert 'ORUN_HOST_SANITIZERS:-1' in host
assert 'sanitizer_flags=(-fsanitize=address,undefined)' in host
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

# CodeQL is a pinned local CLI gate because the hosted GitHub runner failed
# before dispatching any steps. It still observes the canonical host source
# graph and must remain outside the production PlatformIO environment.
assert not (root / ".github/workflows/codeql.yml").exists()
assert 'codeql-bundle-v2.27.1' in codeql_setup
assert '1d380f79896ededc654c7b21fafb3360136f1aeb678ad4df4df9af3910c6b815' in codeql_setup
assert 'sha256sum --check' in codeql_setup
assert 'database create' in codeql_run
assert '--language=c-cpp' in codeql_run
assert 'env ORUN_HOST_SANITIZERS=0 ./firmware/tests/run_host_tests.sh' in codeql_run
assert 'cpp-security-and-quality.qls' in codeql_run
assert '--format=sarif-latest' in codeql_run
assert 'summarize_sarif.py' in codeql_run
assert 'Review every finding before merge' in codeql_summary

for forbidden in (
    '-fsanitize=fuzzer',
    'ORUN_HOST_COVERAGE',
    'ORUN_FUZZ_RUNS',
):
    assert forbidden not in platformio, forbidden

print("DEVQ1 host quality tooling source-contract guards: PASS")
