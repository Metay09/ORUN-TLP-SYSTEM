#!/usr/bin/env bash
# KQ05 prospective Gateway history-region reuse: isolated portable codec only.
# Never links a physical NOR writer, touches a serial port, or erases old data.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  -I firmware/include \
  firmware/src/kq05_gateway_receipt_journal_format.cpp \
  firmware/tests/sf5/test_kq05_gateway_receipt_journal_format.cpp \
  -o "$tmp/receipt-journal-format"

"$tmp/receipt-journal-format"
