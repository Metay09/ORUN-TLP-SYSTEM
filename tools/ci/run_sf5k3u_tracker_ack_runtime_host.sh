#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5k3u_tracker_ack_runtime_source.py
bash tools/ci/run_sf5k3m_release_host.sh
echo 'SF5K3U host checks: PASS (no hardware/LoRa claim)'
