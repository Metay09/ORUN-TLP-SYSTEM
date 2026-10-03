#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../../.."

version="${ORUN_CODEQL_VERSION:-2.27.1}"
default_bin="build/tools/codeql-v$version/codeql/codeql"
codeql_bin="${CODEQL_BIN:-}"

if [[ -z "$codeql_bin" ]]; then
  if [[ -x "$default_bin" ]]; then
    codeql_bin="$default_bin"
  elif command -v codeql >/dev/null 2>&1; then
    codeql_bin="$(command -v codeql)"
  else
    echo "ERROR: CodeQL CLI bundle not found." >&2
    echo "Run ./firmware/tests/codeql/setup_codeql.sh first." >&2
    exit 2
  fi
fi

if [[ ! -x "$codeql_bin" ]]; then
  echo "ERROR: CODEQL_BIN is not executable: $codeql_bin" >&2
  exit 2
fi

out_root="${ORUN_CODEQL_OUTPUT_DIR:-build/codeql}"
db="$out_root/db-cpp"
sarif="$out_root/orun-cpp-security-and-quality.sarif"

rm -rf "$out_root"
mkdir -p "$out_root"

echo "== DEVQ1 local CodeQL: verify CLI =="
"$codeql_bin" version

echo "== DEVQ1 local CodeQL: create C/C++ database from canonical host build graph =="
"$codeql_bin" database create "$db"   --language=c-cpp   --source-root=.   --command="env ORUN_HOST_SANITIZERS=0 ./firmware/tests/run_host_tests.sh"

echo "== DEVQ1 local CodeQL: security-and-quality analysis =="
"$codeql_bin" database analyze "$db"   'codeql/cpp-queries:codeql-suites/cpp-security-and-quality.qls'   --format=sarif-latest   --sarif-category=orun-cpp   --output="$sarif"

python3 firmware/tests/codeql/summarize_sarif.py "$sarif"

echo "ORUN local CodeQL execution: PASS"
echo "SARIF: $sarif"
echo "CodeQL findings are review inputs; this script does not hide or auto-dismiss them."
