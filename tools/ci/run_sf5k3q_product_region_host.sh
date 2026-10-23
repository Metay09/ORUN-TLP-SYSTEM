#!/usr/bin/env bash
# SF5K3Q: independent OCQ2/OCQ1/OBS1 census/ownership host test.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
work="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5k3q.XXXXXXXX")"
trap 'rm -rf -- "$work"' EXIT
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror -Ifirmware/include)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"${CXX:-g++}" "${flags[@]}" \
  firmware/src/product_region.cpp \
  firmware/tests/sf5/test_sf5k3q_product_region.cpp \
  -o "$work/region"
"$work/region"
