#!/usr/bin/env bash
# PC-only bounded 8 KiB fake NOR journal. Does not link any RAK Flash backend.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

g++ -std=c++17 -O1 -Wall -Wextra -Wpedantic -Werror \
  -fno-omit-frame-pointer -fsanitize=address,undefined \
  -I firmware/include \
  firmware/src/kq05_gateway_receipt_journal_format.cpp \
  firmware/src/kq05_gateway_receipt_journal.cpp \
  firmware/tests/sf5/test_kq05_gateway_receipt_journal.cpp \
  -o "$tmp/journal-owner"

"$tmp/journal-owner"
echo "KQ05 GATEWAY 8KiB JOURNAL OWNER PC: PASS"
