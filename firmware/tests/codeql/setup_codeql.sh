#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../../.."

version="${ORUN_CODEQL_VERSION:-2.27.1}"
install_root="${ORUN_CODEQL_HOME:-build/tools/codeql-v$version}"
codeql_bin="$install_root/codeql/codeql"

if [[ "$version" != "2.27.1" ]]; then
  echo "ERROR: DEVQ1 pins CodeQL 2.27.1; override is not supported by the checked-in checksum." >&2
  exit 2
fi

if [[ -x "$codeql_bin" ]]; then
  echo "CodeQL already installed: $codeql_bin"
  "$codeql_bin" version
  exit 0
fi

for tool in curl sha256sum tar; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "ERROR: required tool missing: $tool" >&2
    exit 2
  fi
done

asset="codeql-bundle-linux64.tar.gz"
url="https://github.com/github/codeql-action/releases/download/codeql-bundle-v2.27.1/$asset"
expected_sha256="1d380f79896ededc654c7b21fafb3360136f1aeb678ad4df4df9af3910c6b815"
tmp_dir="$(mktemp -d /tmp/orun-codeql.XXXXXX)"
trap 'rm -rf "$tmp_dir"' EXIT
archive="$tmp_dir/$asset"

echo "== DEVQ1 CodeQL setup: download pinned official bundle v$version =="
echo "This is a one-time host tooling download (~692 MB compressed)."
curl --fail --location --retry 3 --output "$archive" "$url"

printf '%s  %s\n' "$expected_sha256" "$archive" | sha256sum --check -

rm -rf "$install_root"
mkdir -p "$install_root"
tar -xzf "$archive" -C "$install_root"

if [[ ! -x "$codeql_bin" ]]; then
  echo "ERROR: CodeQL executable missing after extraction: $codeql_bin" >&2
  exit 2
fi

echo "== DEVQ1 CodeQL setup: verify bundle =="
"$codeql_bin" version
"$codeql_bin" resolve languages | grep -E '(^|[[:space:]])(c-cpp|cpp)([[:space:]]|$)' >/dev/null
"$codeql_bin" resolve packs | grep 'codeql/cpp-queries' >/dev/null

echo "ORUN local CodeQL setup: PASS"
echo "CodeQL binary: $codeql_bin"
