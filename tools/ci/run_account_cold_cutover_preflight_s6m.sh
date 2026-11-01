#!/usr/bin/env bash
# ORUN S6M: regression tests and READ-ONLY live cutover topology inventory.
# No unit is stopped/started, no SQL file is opened for write, no credentials
# are read, no backup is generated, no migration or APK is performed.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN S6M COLD CUTOVER PREFLIGHT HOST TESTS ==="
PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest -v \
  backend.tests.test_account_cold_cutover_preflight_s6m
echo "ORUN_S6M_PREFLIGHT_HOST_PASS"
echo "=== ORUN S6M LIVE READ-ONLY TOPOLOGY INSPECTION ==="
PYTHONDONTWRITEBYTECODE=1 python3 -B tools/ci/preflight_account_cold_cutover_s6m.py
echo "ORUN_S6M_PRECHECK_COMPLETE"
echo "COLD_CHECKPOINT=NOT_ATTEMPTED"
echo "PRODUCTION_ACTIVATION=NO"
