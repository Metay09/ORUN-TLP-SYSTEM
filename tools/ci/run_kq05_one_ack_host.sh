#!/usr/bin/env bash
# KQ05 opt-in 1..5 single Gateway ACK authority. Host/PC ONLY.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_kq05_unified_ack_source.py
echo "=== KQ05 VERIFIED BACKEND HISTORY RECEIPT SELECTION ==="
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_kq05_backend_adaptive_receipt.py
bash tools/ci/run_kq05_gateway_batch_collector_host.sh
bash tools/ci/run_kq05_gateway_batch_runtime_host.sh
bash tools/ci/run_kq05_tracker_batch_radio_runtime_host.sh
bash tools/ci/run_sf5k3v_tracker_tx_host.sh
echo "KQ05 unified 1..5 Gateway ACK host gates complete"
echo "NO BOARD FLASH / NO REAL LORA / NO PRODUCTION MERGE"
