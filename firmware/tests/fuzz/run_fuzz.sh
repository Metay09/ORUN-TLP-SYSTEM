#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../../.."

if ! command -v clang++ >/dev/null 2>&1; then
  echo "ERROR: clang++ with libFuzzer support is required." >&2
  echo "Install Debian clang, then rerun this script." >&2
  exit 2
fi

runs="${ORUN_FUZZ_RUNS:-10000}"
build_root="${ORUN_FUZZ_BUILD_DIR:-build/fuzz}"
bin_dir="$build_root/bin"
corpus_root="$build_root/corpus"
artifact_root="$build_root/artifacts"

rm -rf "$build_root"
mkdir -p "$bin_dir" "$corpus_root" "$artifact_root"

flags=(
  -std=c++17 -O1 -g -Wall -Wextra -Werror
  -fno-omit-frame-pointer
  -fsanitize=fuzzer,address,undefined
  -Ifirmware/include
)

build_target() {
  local name="$1"
  shift
  clang++ "${flags[@]}" "$@" -o "$bin_dir/$name"
}

build_target tlp_position   firmware/tests/fuzz/fuzz_tlp_position.cpp   firmware/src/tlp_position_packet.cpp

build_target config_format   firmware/tests/fuzz/fuzz_config_format.cpp   firmware/src/config_format.cpp firmware/src/journal_format.cpp   firmware/src/tlp_position_packet.cpp

build_target security_format   firmware/tests/fuzz/fuzz_security_format.cpp   firmware/src/security_format.cpp firmware/src/journal_format.cpp   firmware/src/tlp_position_packet.cpp

build_target geofence_format   firmware/tests/fuzz/fuzz_geofence_format.cpp   firmware/src/geofence_format.cpp firmware/src/geofence_geometry.cpp   firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp

for target in tlp_position config_format security_format geofence_format; do
  mkdir -p "$corpus_root/$target" "$artifact_root/$target"
done

python3 - "$corpus_root" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
(root / "tlp_position" / "valid_zero.bin").write_bytes(bytes([1, 2]) + bytes(32))
seed = bytes(range(64))
for name in ("config_format", "security_format", "geofence_format"):
    (root / name / "seed.bin").write_bytes(seed)
PY

run_target() {
  local name="$1"
  local max_len="$2"
  echo "== libFuzzer: $name ($runs runs) =="
  "$bin_dir/$name" "$corpus_root/$name"     -runs="$runs"     -max_len="$max_len"     -timeout=5     -artifact_prefix="$artifact_root/$name/"
}

run_target tlp_position 64
run_target config_format 128
run_target security_format 128
run_target geofence_format 600

echo "ORUN bounded host fuzz smoke: PASS"
echo "Artifacts (only on failures/findings): $artifact_root"
