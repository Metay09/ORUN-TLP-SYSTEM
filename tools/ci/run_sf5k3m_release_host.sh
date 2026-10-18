#!/usr/bin/env bash
# SF5K3M new only: real SecurityStore + real ProductRecordStore +
# asynchronous ObservationStore custody-release authority.
# No re-run of previous SF5K3I/J/K/L host suite.
set -euo pipefail
cd "$(dirname "$0")/../.."
dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5k3m.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT
cxx="${CXX:-g++}"
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
       -Wno-conversion -Wno-sign-conversion
       -DORUN_SF5K3L_HOST_TEST -Ifirmware/include -Ifirmware/tests/sf5)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
echo "[SF5K3M] actual Store post-HMAC exact-object release, flash/reboot"
"$cxx" "${flags[@]}" \
  firmware/tests/sf5/test_sf5k3m_authenticated_release.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/src/custody_ack_release_owner.cpp \
  firmware/src/custody_ack_product_bridge.cpp \
  firmware/src/custody_ack_verifier.cpp firmware/src/custody_ack.cpp \
  firmware/src/security_store.cpp firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp firmware/src/security_v3_format.cpp \
  firmware/src/security_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  firmware/src/product_record_store.cpp firmware/src/product_region.cpp \
  firmware/src/observation_store.cpp firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  firmware/src/product_period_builder.cpp firmware/src/tlp_v2_product_secure.cpp \
  -lcrypto -o "$dir/sf5k3m"
"$dir/sf5k3m"
