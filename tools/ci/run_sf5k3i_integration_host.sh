#!/usr/bin/env bash
# SF5K3I focused REAL SecurityStore integration check (host-only).
# Compile exactly one new integrated test; don't rerun earlier SF5K3A-H.
set -euo pipefail
cd "$(dirname "$0")/../.."
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5k3i.XXXXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT
cxx="${CXX:-g++}"
flags=(
  -std=gnu++11 -O1 -g -Wall -Wextra -Werror
  -Wconversion -Wsign-conversion
  -Wno-conversion -Wno-sign-conversion
  -Ifirmware/include -Ifirmware/tests/sf5
)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
echo "[SF5K3I] REAL SecurityStore v3 integration only ($cxx)"
"$cxx" "${flags[@]}" \
  firmware/tests/sf5/test_sf5k3i_real_security_store_v3.cpp \
  firmware/src/security_store.cpp \
  firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp \
  firmware/src/security_v3_format.cpp \
  firmware/tests/sf5/sf5k3c_security_v3_candidate.cpp \
  firmware/tests/sf5/sf5k3d_security_v3_snapshot_candidate.cpp \
  firmware/src/security_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -o "$build_dir/sf5k3i"
"$build_dir/sf5k3i"
