#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
dir="$(mktemp -d /tmp/orun-sf5k3o.XXXXXX)"
trap 'rm -rf "$dir"' EXIT
"${CXX:-g++}" -std=gnu++11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -Ifirmware/include \
  firmware/src/custody_store_product_format.cpp \
  firmware/tests/sf5/test_sf5k3o_custody_product_format.cpp \
  -o "$dir/sf5k3o"
"$dir/sf5k3o"
