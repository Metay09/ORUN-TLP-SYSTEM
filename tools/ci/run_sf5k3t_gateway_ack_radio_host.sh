#!/usr/bin/env bash
# SF5K3T: source-contract assertions plus existing real custody flash/HMAC host test.
# Does not emulate a full RadioManager/SX1262 driver or claim RF hardware success.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
python3 firmware/tests/sf5/test_sf5k3t_gateway_ack_radio_source.py
bash tools/ci/run_sf5k3s_gateway_authorized_ingress_host.sh
echo 'SF5K3T pre-hardware host checks: PASS (no real RF or target build claim)'
