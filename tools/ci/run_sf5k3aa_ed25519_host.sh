#!/usr/bin/env bash
# Host: production-target adapter compiled against a HEADER-SHAPE fake CC310;
# mock CRYS_ECEDW_Verify uses real OpenSSL Ed25519 verification.
# Target probe separately compiles the actual pinned Nordic headers/library.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5k3aa_cc310_source.py
dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5k3aa.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
       -DORUN_SF5K3AA_TARGET_CRYPTO_PROBE=1
       -Ifirmware/tests/sf5/stubs
       -Ifirmware/include)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"${CXX:-g++}" "${flags[@]}" \
  firmware/tests/sf5/test_sf5k3aa_cc310_ed25519_adapter.cpp \
  firmware/src/gateway_ed25519_cc310_verifier.cpp \
  -lcrypto -o "$dir/ed25519"
"$dir/ed25519"
