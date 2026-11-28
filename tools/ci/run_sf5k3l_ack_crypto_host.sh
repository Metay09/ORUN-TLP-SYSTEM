#!/usr/bin/env bash
# Only SF5K3L new crypto verifier; no repeated SF5K3A-K gates.
# Host fixture uses OpenSSL to independently check production-port bytes.
set -euo pipefail
cd "$(dirname "$0")/../.."
dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5k3l.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT
cxx="${CXX:-g++}"

case "${ORUN_ACK_TEST_PROFILE:-product}" in
  product)
    ack_profile_flags=(-DORUN_GATEWAY_BATCH_ACK=1)
    ;;
  standard)
    ack_profile_flags=(-DORUN_TRACKER_BATCH_ACK=1)
    ;;
  pilot)
    ack_profile_flags=(-DORUN_TRACKER_BATCH_ACK=1
                       -DORUN_SF5K3V_TRACKER_TX_PILOT)
    ;;
  *)
    echo "Unknown ACK test profile" >&2
    exit 2
    ;;
esac

flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
       -Wno-conversion -Wno-sign-conversion
       "${ack_profile_flags[@]}" -DORUN_KQ05_TRACKER_BATCH_VERIFY_HOST_TEST=1 -Ifirmware/include -Ifirmware/tests/sf5)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
echo "[SF5K3L] actual SecurityStore scoped ACK SHA256/HKDF/HMAC, independent OpenSSL"
"$cxx" "${flags[@]}" \
  firmware/tests/sf5/test_sf5k3l_custody_ack_crypto.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/src/kq05_tracker_batch_release_owner.cpp \
  firmware/src/kq05_tracker_batch_ack_verifier.cpp \
  firmware/src/kq05_gateway_batch_custody_crypto.cpp \
  firmware/src/kq05_gateway_batch_custody_ack.cpp \
  firmware/src/product_record_store.cpp \
  firmware/src/product_region.cpp \
  firmware/src/observation_store.cpp \
  firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  firmware/src/product_period_builder.cpp \
  firmware/src/security_store.cpp firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp firmware/src/security_v3_format.cpp \
  firmware/src/tlp_v2_product_secure.cpp firmware/src/security_format.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  -lcrypto -o "$dir/sf5k3l"
"$dir/sf5k3l"
