#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../../.."
repo_root="$(pwd -P)"
build_root_canonical="$(realpath -m -- "$repo_root/build")"

safe_build_child() {
  local requested="$1"
  local resolved
  resolved="$(realpath -m -- "$requested")"
  if [[ "$resolved" != "$build_root_canonical/"* ]]; then
    echo "ERROR: fuzz output must be a child of $build_root_canonical: $requested" >&2
    exit 2
  fi
  printf '%s\n' "$resolved"
}

if ! command -v clang++ >/dev/null 2>&1; then
  echo "ERROR: clang++ with libFuzzer support is required." >&2
  echo "Install Debian clang, then rerun this script." >&2
  exit 2
fi

runs="${ORUN_FUZZ_RUNS:-10000}"
case "$runs" in
  ''|*[!0-9]*)
    echo "ERROR: ORUN_FUZZ_RUNS must be a positive integer." >&2
    exit 2
    ;;
esac
if [[ "$runs" -lt 1 ]]; then
  echo "ERROR: ORUN_FUZZ_RUNS must be >= 1." >&2
  exit 2
fi

build_root="$(safe_build_child "${ORUN_FUZZ_BUILD_DIR:-build/fuzz}")"
bin_dir="$build_root/bin"
corpus_root="$build_root/corpus"
artifact_root="$build_root/artifacts"

# Rebuild binaries every run, but preserve corpus and crash artifacts so a
# discovered reproducer is not erased by the next invocation.
rm -rf -- "$bin_dir"
mkdir -p -- "$bin_dir" "$corpus_root" "$artifact_root"

flags=(
  -std=c++17 -O1 -g -Wall -Wextra -Werror
  -fno-omit-frame-pointer
  -fsanitize=fuzzer,address,undefined
  -fno-sanitize-recover=undefined
  -Ifirmware/include
)

build_target() {
  local name="$1"
  shift
  clang++ "${flags[@]}" "$@" -o "$bin_dir/$name"
}

build_target tlp_position \
  firmware/tests/fuzz/fuzz_tlp_position.cpp \
  firmware/src/tlp_position_packet.cpp

build_target config_format \
  firmware/tests/fuzz/fuzz_config_format.cpp \
  firmware/src/config_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp

build_target security_format \
  firmware/tests/fuzz/fuzz_security_format.cpp \
  firmware/src/security_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp

build_target geofence_format \
  firmware/tests/fuzz/fuzz_geofence_format.cpp \
  firmware/src/geofence_format.cpp firmware/src/geofence_geometry.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp

for target in tlp_position config_format security_format geofence_format; do
  mkdir -p -- "$corpus_root/$target" "$artifact_root/$target"
done

python3 - "$corpus_root" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
(root / "tlp_position" / "valid_zero.bin").write_bytes(bytes([1, 2]) + bytes(32))
(root / "config_format" / "seed.bin").write_bytes(bytes(range(128)))
(root / "security_format" / "seed.bin").write_bytes(bytes(range(128)))
# Force libFuzzer to exercise the fixed-size 564-byte raw geofence record path
# from the first corpus load instead of waiting for length growth.
(root / "geofence_format" / "full_record_seed.bin").write_bytes(
    bytes(i & 0xFF for i in range(564))
)
PY

run_target() {
  local name="$1"
  local max_len="$2"
  echo "== libFuzzer: $name ($runs runs) =="
  "$bin_dir/$name" "$corpus_root/$name" \
    -runs="$runs" \
    -max_len="$max_len" \
    -timeout=5 \
    -artifact_prefix="$artifact_root/$name/"
}

run_target tlp_position 64
run_target config_format 128
run_target security_format 128
run_target geofence_format 600

echo "ORUN bounded host fuzz smoke: PASS"
echo "Persistent corpus: $corpus_root"
echo "Persistent failure artifacts: $artifact_root"
