#!/usr/bin/env bash
# ORUN S6N - only synthetic service-controller and SQLite backup tests,
# followed by the CLI's DEFAULT READ-ONLY PLAN inspection on user's PC.
# This script contains NO --execute or service stop/start.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN S6N SYNTHETIC COLD CHECKPOINT ACCEPTANCE ==="
PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest -v \
  backend.tests.test_account_cold_checkpoint_s6n
echo "ORUN_S6N_COLD_CHECKPOINT_HOST_PASS"
echo "=== ORUN S6N ACTUAL PC READ-ONLY PLAN ==="
PYTHONDONTWRITEBYTECODE=1 python3 -B tools/ci/cold_checkpoint_shared_s6n.py
echo "ORUN_S6N_PLAN_ONLY_PASS"
echo "LIVE_SERVICE_PAUSE=NOT_ATTEMPTED"
echo "LIVE_BACKUP=NOT_CREATED"
echo "PRODUCTION_APPROVAL=NO"
