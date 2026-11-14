#!/usr/bin/env bash
# KQ05 draft: PC-only host regressions + isolated lab firmware build.
# NO RAK flashing, no service/DB mutation and no GitHub CI.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
echo "=== KQ05 SOURCE ID ==="
git rev-parse HEAD
git diff --check
echo "=== KQ05 1/5 TRACKER LIVE/HISTORY ==="
bash tools/ci/run_sf5k3v_tracker_tx_host.sh
echo "=== KQ05 2/5 GATEWAY NONCUSTODY ==="
bash tools/ci/run_sf5k3w_gateway_product_runtime_host.sh
echo "=== KQ05 3/5 BACKEND AUTHENTICATED RECEIPT ==="
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_kq05_backend_batch_receipt.py
echo "=== KQ05 4/5 TRACKER RECEIPT/FLASH ==="
bash tools/ci/run_kq05_backend_receipt_release_host.sh
echo "=== KQ05 5/5 ISOLATED RAK4630 LAB BUILD ==="
(
  cd firmware
  pio run -e rak4630_kq05_lab
)
if [[ "${ORUN_KQ05_FULL_HOST:-0}" == 1 ]]; then
  echo "=== OPTIONAL FULL PC HOST SUITE ==="
  bash firmware/tests/run_host_tests.sh
fi
echo "KQ05 PC PREFLIGHT PASS (NOT FIELD RELEASE)"
