#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../../.."
repo_root="$(pwd -P)"
build_root="$(realpath -m -- "$repo_root/build")"

safe_build_child() {
  local requested="$1"
  local resolved
  resolved="$(realpath -m -- "$requested")"
  if [[ "$resolved" != "$build_root/"* ]]; then
    echo "ERROR: CodeQL output must be a child of $build_root: $requested" >&2
    exit 2
  fi
  printf '%s\n' "$resolved"
}

version="2.27.1"
asset="codeql-bundle-linux64.tar.gz"
expected_sha256="1d380f79896ededc654c7b21fafb3360136f1aeb678ad4df4df9af3910c6b815"
install_root="$(safe_build_child "${ORUN_CODEQL_HOME:-build/tools/codeql-v$version}")"
codeql_bin="$install_root/codeql/codeql"
marker="$install_root/.orun-codeql-verified"
bundle_archive="$install_root/$asset"

if [[ -n "${ORUN_CODEQL_VERSION:-}" && "${ORUN_CODEQL_VERSION}" != "$version" ]]; then
  echo "ERROR: DEVQ1 pins CodeQL $version; ORUN_CODEQL_VERSION override is not allowed." >&2
  exit 2
fi
if [[ -n "${CODEQL_BIN:-}" ]]; then
  echo "ERROR: DEVQ1 does not accept CODEQL_BIN overrides; use the verified pinned bundle." >&2
  exit 2
fi
if [[ ! -x "$codeql_bin" || ! -f "$marker" || ! -f "$bundle_archive" ]]; then
  echo "ERROR: verified CodeQL bundle not found. Run ./firmware/tests/codeql/setup_codeql.sh." >&2
  exit 2
fi
grep -Fxq "version=$version" "$marker" || {
  echo "ERROR: CodeQL verification marker has wrong version." >&2
  exit 2
}
grep -Fxq "sha256=$expected_sha256" "$marker" || {
  echo "ERROR: CodeQL verification marker has wrong checksum." >&2
  exit 2
}
python3 firmware/tests/codeql/verify_bundle_install.py \
  "$bundle_archive" "$codeql_bin" "$expected_sha256" || {
  echo "ERROR: CodeQL install does not match the pinned checksum-verified bundle." >&2
  exit 2
}
actual_version="$("$codeql_bin" version --format=terse | head -n 1 | tr -d '\r')"
if [[ "$actual_version" != "$version" ]]; then
  echo "ERROR: CodeQL runtime version $actual_version does not match pinned $version." >&2
  exit 2
fi

out_root="$(safe_build_child "${ORUN_CODEQL_OUTPUT_DIR:-build/codeql}")"
db="$out_root/db-cpp"
sarif="$out_root/orun-cpp-security-and-quality.sarif"

rm -rf -- "$out_root"
mkdir -p -- "$out_root"

echo "== DEVQ1 local CodeQL: verify CLI =="
"$codeql_bin" version

echo "== DEVQ1 local CodeQL: create C/C++ database from canonical host build graph =="
"$codeql_bin" database create "$db" \
  --language=c-cpp \
  --source-root=. \
  --command="env ORUN_HOST_SANITIZERS=0 ./firmware/tests/run_host_tests.sh"

echo "== DEVQ1 local CodeQL: security-and-quality analysis =="
"$codeql_bin" database analyze "$db" \
  'codeql/cpp-queries:codeql-suites/cpp-security-and-quality.qls' \
  --format=sarif-latest \
  --sarif-category=orun-cpp \
  --output="$sarif"

python3 firmware/tests/codeql/summarize_sarif.py "$sarif"

echo "ORUN local CodeQL execution: PASS"
echo "SARIF: $sarif"
echo "CodeQL findings are review inputs; this script does not hide or auto-dismiss them."
