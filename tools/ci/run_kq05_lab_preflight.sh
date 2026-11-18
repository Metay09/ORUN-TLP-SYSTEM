#!/usr/bin/env bash
# KQ05 draft: PC-only host regressions + isolated lab firmware build.
# NO RAK flashing, no service/DB mutation and no GitHub CI.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
echo "=== KQ05 ONE PERIODIC OWNER / LEGACY DUAL WRITER OFF ==="
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_kq05_single_product_owner_source.py
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_kq05_flash_audit.py
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_kq05_pilot_trim_source.py
echo "=== KQ05 SOURCE ID ==="
git rev-parse HEAD
git diff --check
echo "=== KQ05 1/5 TRACKER LIVE/HISTORY ==="
bash tools/ci/run_sf5k3v_tracker_tx_host.sh
echo "=== KQ05 CRITICAL EVENT -> EXISTING CUSTODY ACK (PC GATE) ==="
bash tools/ci/run_kq05_critical_event_tx_host.sh
echo "=== KQ05 2/5 GATEWAY NONCUSTODY ==="
bash tools/ci/run_sf5k3w_gateway_product_runtime_host.sh
echo "=== KQ05 3/5 BACKEND AUTHENTICATED RECEIPT ==="
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_kq05_backend_batch_receipt.py
echo "=== KQ05 BACKEND DURABLE RECEIPT OUTBOX ==="
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_kq05_backend_receipt_outbox.py
echo "=== KQ05 AUTHENTICATED BACKEND-EDGE RECEIPT PAGES ==="
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_kq05_receipt_edge_transport.py
echo "=== KQ05 EDGE-GATEWAY AUTHENTICATED DURABLE STAGE ==="
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_kq05_edge_gateway_stage.py
echo "=== KQ05 REAL LOGICAL BLE 0x91 -> DURABLE GATEWAY -> 0x92 ==="
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_kq05_gateway_ble_durable_stage_bridge.py
echo "=== KQ05 GATEWAY DURABLE STAGE RESULT, NOT TRACKER ACK ==="
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_kq05_gateway_stage_ack.py
echo "=== KQ05 NATIVE BLE WIRE / FAIL-CLOSED INGRESS CONTRACT ==="
bash tools/ci/run_kq05_edge_link_wire_host.sh
echo "=== KQ05 GATEWAY NATIVE HMAC RAM RELAY / ACK-FIRST ELIGIBILITY ==="
bash tools/ci/run_kq05_gateway_ram_relay_host.sh
echo "=== KQ05 SIGNED OGK1 V2 LINK / EPOCH / EDGE DERIVATION ==="
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_kq05_gateway_link_derive.py
bash tools/ci/run_kq05_signed_link_credentials_host.sh
echo "=== KQ05 PYTHON BACKEND->EDGE REAL OFFER -> NATIVE BLE GATEWAY RAM ==="
bash tools/ci/run_kq05_real_byte_interop_host.sh
echo "=== KQ05 DURABLE GATEWAY STAGED -> NATIVE RF PRIORITY/WINDOW/BUDGET ==="
bash tools/ci/run_kq05_durable_stage_rf_policy_host.sh
echo "=== KQ05 GATEWAY SINGLE-COPY 8KiB RECEIPT NOR FORMAT (NO FLASH WRITER) ==="
bash tools/ci/run_kq05_receipt_journal_format_host.sh
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
