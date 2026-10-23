#!/usr/bin/env bash
# SF5K3J — one new real SecurityStore Gateway policy test (host only).
# Does NOT re-run SF5K3A-I, does not flash physical devices.
set -euo pipefail
cd "$(dirname "$0")/../.."
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5k3j.XXXXXXXX")"
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
echo "[SF5K3J] real SecurityStore persistent Gateway grant/revoke/floor"
"$cxx" "${flags[@]}" \
  firmware/tests/sf5/test_sf5k3j_real_security_v3_policy.cpp \
  firmware/src/security_store.cpp \
  firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp \
  firmware/src/security_v3_format.cpp \
  firmware/src/security_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -o "$build_dir/sf5k3j"
"$build_dir/sf5k3j"
