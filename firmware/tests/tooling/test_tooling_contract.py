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
codeql_verify = (root / "firmware/tests/codeql/verify_bundle_install.py").read_text(encoding="utf-8")
security_fuzz = (root / "firmware/tests/fuzz/fuzz_security_format.cpp").read_text(encoding="utf-8")
gitignore = (root / ".gitignore").read_text(encoding="utf-8")

# Host sanitizers remain default-on and UBSan must be fatal. The only explicit
# sanitizer-off execution belongs to the CodeQL preload-traced build.
assert 'ORUN_HOST_SANITIZERS:-1' in host
assert '-fsanitize=address,undefined -fno-sanitize-recover=undefined' in host
assert 'ORUN_HOST_SANITIZERS must be exactly 0 or 1' in host
assert 'ORUN_HOST_SANITIZERS=0' not in coverage
assert 'ORUN_HOST_SANITIZERS=0' not in fuzz
assert 'ORUN_HOST_SANITIZERS=0' not in codeql_setup
assert codeql_run.count('ORUN_HOST_SANITIZERS=0') == 1

# Coverage is opt-in, diagnostic-only, and includes source plus production
# header-only logic. No percentage gate is allowed.
assert 'ORUN_HOST_COVERAGE:-0' in host
assert 'coverage_flags=(--coverage -fprofile-abs-path)' in host
assert 'ORUN_HOST_COVERAGE=1' in coverage
assert "--filter 'firmware/src/'" in coverage
assert "--filter 'firmware/include/'" in coverage
assert '--html-details' in coverage
assert '--fail-under' not in coverage
assert 'safe_build_child' in coverage

# Initial fuzzing is host-only, bounded, sanitizer-backed, fatal on UBSan, and
# preserves corpus/artifacts across runs.
assert '-fsanitize=fuzzer,address,undefined' in fuzz
assert '-fno-sanitize-recover=undefined' in fuzz
assert 'ORUN_FUZZ_RUNS:-10000' in fuzz
assert 'full_record_seed.bin' in fuzz
assert 'rm -rf -- "$bin_dir"' in fuzz
assert 'rm -rf -- "$build_root"' not in fuzz
for target in (
    'tlp_position',
    'config_format',
    'security_format',
    'geofence_format',
):
    assert f'build_target {target}' in fuzz

# CodeQL is local because the hosted runner failed before dispatch. The local
# path must use the checksum-verified pinned 2.27.1 bundle and safe build paths.
assert not (root / ".github/workflows/codeql.yml").exists()
assert 'version="2.27.1"' in codeql_setup
assert 'version="2.27.1"' in codeql_run
assert 'codeql-bundle-v2.27.1' in codeql_setup
assert '1d380f79896ededc654c7b21fafb3360136f1aeb678ad4df4df9af3910c6b815' in codeql_setup
assert 'sha256sum --check' in codeql_setup
assert '.orun-codeql-verified' in codeql_setup
assert '.orun-codeql-verified' in codeql_run
assert 'version --format=terse' in codeql_setup
assert 'version --format=terse' in codeql_run
assert 'CODEQL_BIN overrides' in codeql_run
assert 'database create' in codeql_run
assert '--language=c-cpp' in codeql_run
assert 'env ORUN_HOST_SANITIZERS=0 ./firmware/tests/run_host_tests.sh' in codeql_run
assert 'cpp-security-and-quality.qls' in codeql_run
assert '--format=sarif-latest' in codeql_run
assert 'summarize_sarif.py' in codeql_run
assert 'codeql-bundle-linux64.tar.gz' in codeql_setup
assert 'codeql-bundle-linux64.tar.gz' in codeql_run
assert 'verify_bundle_install.py' in codeql_setup
assert 'verify_bundle_install.py' in codeql_run
assert 'sha256_archive_member' in codeql_verify
assert 'differs from checksum-verified bundle' in codeql_verify
assert 'test_verify_bundle_install.py' in host

# SARIF severity must honor kind, rule/component indexes and rule defaults.
# Non-fail results have no severity; fail results default to warning only when
# neither the result nor the reporting descriptor supplies a level.
assert 'result.get("kind", "fail")' in codeql_summary
assert 'return "none"' in codeql_summary
assert 'defaultConfiguration' in codeql_summary
assert 'return "warning"' in codeql_summary
assert 'toolComponent' in codeql_summary
assert 'tool.get("extensions", [])' in codeql_summary

# Security fuzz semantic mutation offsets are derived and compile-time guarded
# against record-size/layout drift instead of being unguarded numeric literals.
for guarded_offset in (
    'kPageHeaderCrcOffset',
    'kCredentialCrcOffset',
    'kTxReserveCrcOffset',
    'kSecurityStateCrcOffset',
):
    assert guarded_offset in security_fuzz
assert security_fuzz.count('static_assert(') >= 8

# User-controlled destructive output paths are constrained below root /build/.
assert 'safe_build_child' in host
assert 'safe_build_child' in fuzz
assert 'safe_build_child' in codeql_setup
assert 'safe_build_child' in codeql_run
assert '/build/' in gitignore
assert '\nbuild/\n' not in gitignore

for forbidden in (
    '-fsanitize=fuzzer',
    '-fno-sanitize-recover',
    'ORUN_HOST_COVERAGE',
    'ORUN_FUZZ_RUNS',
    'ORUN_HOST_SANITIZERS',
):
    assert forbidden not in platformio, forbidden

print("DEVQ1 host quality tooling source-contract guards: PASS")
