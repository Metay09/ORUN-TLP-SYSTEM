#!/usr/bin/env bash
# Standalone developer check for SF5K3A; host only, NEVER product radio.
set -euo pipefail
cd "$(dirname "$0")/../.."
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5k3a.XXXXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT

cxx="${CXX:-g++}"
flags=(
  -std=gnu++11 -O1 -g -Wall -Wextra -Werror
  -Wconversion -Wsign-conversion -Ifirmware/include -Ifirmware/tests/sf5
)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi

echo "[SF5K3A] host model ($cxx)"
"$cxx" "${flags[@]}" \
  firmware/tests/sf5/test_sf5k3_release_progress_candidate.cpp \
  firmware/tests/sf5/sf5k3_release_progress_candidate.cpp \
  -o "$build_dir/model"
"$build_dir/model"

echo "[SF5K3A] actual ObservationStore ($cxx)"
"$cxx" "${flags[@]}" \
  firmware/tests/sf5/test_sf5k3_observation_store_port.cpp \
  firmware/tests/sf5/sf5k3_observation_store_port.cpp \
  firmware/tests/sf5/sf5k3_release_progress_candidate.cpp \
  firmware/src/observation_store.cpp \
  firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  -o "$build_dir/actual"
"$build_dir/actual"

echo "[SF5K3A] real ProductRecordStore read-only custody preflight ($cxx)"
# Match the existing SF5F fixture's compiler-warning contract; unlike the
# new SF5K3A-only test sources it was not originally -Wconversion clean.
"$cxx" "${flags[@]}" -Wno-conversion -Wno-sign-conversion \
  firmware/tests/sf5/test_sf5f_product_record_store.cpp \
  firmware/src/product_record_store.cpp \
  firmware/src/product_region.cpp \
  firmware/src/observation_store.cpp \
  firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  firmware/src/product_period_builder.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  -o "$build_dir/product-store"
"$build_dir/product-store" --custody-preflight-only

echo "[SF5K3B] 0x05 single-object SHA256/HKDF/HMAC host oracle (test-only)"
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5k3b_single_ack_reference.py
echo "[SF5K3C] candidate v3 custody-policy journal (host-only)"
"$cxx" "${flags[@]}" -Wno-conversion -Wno-sign-conversion \
  firmware/tests/sf5/test_sf5k3c_security_v3_candidate.cpp \
  firmware/tests/sf5/sf5k3c_security_v3_candidate.cpp \
  firmware/src/security_format.cpp \
  firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -o "$build_dir/v3-candidate"
"$build_dir/v3-candidate"

echo "[SF5K3D] v2->v3 and v3->v3 A/B snapshot/byte-cut (host only)"
"$cxx" "${flags[@]}" -Wno-conversion -Wno-sign-conversion \
  firmware/tests/sf5/test_sf5k3d_security_v3_snapshot_candidate.cpp \
  firmware/tests/sf5/sf5k3d_security_v3_snapshot_candidate.cpp \
  firmware/tests/sf5/sf5k3c_security_v3_candidate.cpp \
  firmware/src/security_format.cpp \
  firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -o "$build_dir/v3-snapshot"
"$build_dir/v3-snapshot"

echo "[SF5K3A/SF5K3B/SF5K3C/SF5K3D] all focused host tests PASS"
