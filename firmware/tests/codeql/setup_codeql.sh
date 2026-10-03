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
    echo "ERROR: CodeQL install path must be a child of $build_root: $requested" >&2
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
url="https://github.com/github/codeql-action/releases/download/codeql-bundle-v2.27.1/$asset"
api_asset_url="https://api.github.com/repos/github/codeql-action/releases/assets/572325752"

if [[ -n "${ORUN_CODEQL_VERSION:-}" && "${ORUN_CODEQL_VERSION}" != "$version" ]]; then
  echo "ERROR: DEVQ1 pins CodeQL $version; ORUN_CODEQL_VERSION override is not allowed." >&2
  exit 2
fi

codeql_terse_version() {
  "$1" version --format=terse | head -n 1 | tr -d '\r'
}

verified_install() {
  [[ -x "$codeql_bin" && -f "$marker" && -f "$bundle_archive" ]] || return 1
  grep -Fxq "version=$version" "$marker" || return 1
  grep -Fxq "sha256=$expected_sha256" "$marker" || return 1
  python3 firmware/tests/codeql/verify_bundle_install.py \
    "$bundle_archive" "$codeql_bin" "$expected_sha256" >/dev/null || return 1
  [[ "$(codeql_terse_version "$codeql_bin")" == "$version" ]] || return 1
  "$codeql_bin" resolve languages | grep -E '(^|[[:space:]])(c-cpp|cpp)([[:space:]]|$)' >/dev/null
  "$codeql_bin" resolve packs | grep 'codeql/cpp-queries' >/dev/null
}

if verified_install; then
  echo "CodeQL verified install already present: $codeql_bin"
  "$codeql_bin" version
  echo "ORUN local CodeQL setup: PASS"
  exit 0
fi

for tool in curl sha256sum tar python3; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "ERROR: required tool missing: $tool" >&2
    exit 2
  fi
done

tmp_dir="$(mktemp -d /tmp/orun-codeql.XXXXXX)"
trap 'rm -rf "$tmp_dir"' EXIT
archive="$tmp_dir/$asset"

echo "== DEVQ1 CodeQL setup: download pinned official bundle v$version =="
echo "A missing/invalid verification marker forces a fresh checksum-verified install."
download_asset() {
  local source_url="$1"
  shift
  curl --fail --location \
    --connect-timeout 30 \
    --retry 10 \
    --retry-all-errors \
    --retry-delay 3 \
    --retry-max-time 300 \
    "$@" \
    --output "$archive" \
    "$source_url"
}

if ! download_asset "$url"; then
  echo "Primary GitHub release URL failed; trying the official REST asset endpoint." >&2
  rm -f -- "$archive"
  download_asset "$api_asset_url" \
    -H 'Accept: application/octet-stream' \
    -H 'X-GitHub-Api-Version: 2022-11-28'
fi

printf '%s  %s\n' "$expected_sha256" "$archive" | sha256sum --check -

rm -rf -- "$install_root"
mkdir -p -- "$install_root"
cp -- "$archive" "$bundle_archive"
tar -xzf "$bundle_archive" -C "$install_root"

if [[ ! -x "$codeql_bin" ]]; then
  echo "ERROR: CodeQL executable missing after extraction: $codeql_bin" >&2
  exit 2
fi
if [[ "$(codeql_terse_version "$codeql_bin")" != "$version" ]]; then
  echo "ERROR: extracted CodeQL version does not match pinned $version." >&2
  exit 2
fi

echo "== DEVQ1 CodeQL setup: verify bundle =="
"$codeql_bin" version
"$codeql_bin" resolve languages | grep -E '(^|[[:space:]])(c-cpp|cpp)([[:space:]]|$)' >/dev/null
"$codeql_bin" resolve packs | grep 'codeql/cpp-queries' >/dev/null

cat >"$marker" <<EOF
version=$version
sha256=$expected_sha256
asset_id=572325752
EOF

if ! verified_install; then
  echo "ERROR: CodeQL verification marker/install self-check failed." >&2
  exit 2
fi

echo "ORUN local CodeQL setup: PASS"
echo "CodeQL binary: $codeql_bin"
