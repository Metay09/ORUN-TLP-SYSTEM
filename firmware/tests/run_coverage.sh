#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."
repo_root="$(pwd -P)"
build_root="$(realpath -m -- "$repo_root/build")"

safe_build_child() {
  local requested="$1"
  local resolved
  resolved="$(realpath -m -- "$requested")"
  if [[ "$resolved" != "$build_root/"* ]]; then
    echo "ERROR: coverage output must be a child of $build_root: $requested" >&2
    exit 2
  fi
  printf '%s\n' "$resolved"
}

if ! command -v gcovr >/dev/null 2>&1; then
  echo "ERROR: gcovr is required for host coverage reporting." >&2
  echo "Install it with your Debian package manager, then rerun this script." >&2
  exit 2
fi

coverage_root="$(safe_build_child "${1:-build/host-coverage}")"
object_dir="$coverage_root/objects"

rm -rf -- "$coverage_root"
mkdir -p -- "$object_dir"

echo "== ORUN host coverage: running the existing full host suite =="
ORUN_HOST_COVERAGE=1 \
ORUN_HOST_TEST_DIR="$object_dir" \
  ./firmware/tests/run_host_tests.sh

echo "== ORUN host coverage: generating report =="
gcovr \
  --root . \
  --object-directory "$object_dir" \
  --filter 'firmware/src/' \
  --filter 'firmware/include/' \
  --exclude 'firmware/tests/' \
  --print-summary \
  --html-details "$coverage_root/index.html"

echo "Coverage report: $coverage_root/index.html"
echo "Coverage is diagnostic evidence only; no percentage threshold is a merge gate yet."
