#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
test_dir=$(mktemp -d /tmp/orun-host-tests.XXXXXX)
flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
       -Ifirmware/tests/m3/stubs -Ifirmware/include)
g++ "${flags[@]}" firmware/tests/m3/test_m3.cpp firmware/src/gnss_manager.cpp -o "$test_dir/m3"
"$test_dir/m3"
g++ "${flags[@]}" firmware/tests/m4/test_m4.cpp firmware/src/history_store.cpp \
  firmware/src/journal_format.cpp firmware/src/position_flow.cpp \
  firmware/src/tlp_position_packet.cpp -o "$test_dir/m4"
"$test_dir/m4"
