#!/usr/bin/env bash
# S6G: isolated unit tests + READ-ONLY live inventory. No deployment.
# The unit tests write only to TemporaryDirectory and remove it on exit.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN S6G READ-ONLY PREFLIGHT TESTS ==="
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v \
  backend.tests.test_account_rollout_readiness
echo "ORUN_S6G_AUDIT_UNIT_TESTS_PASS"
echo "=== LIVE SERVICE / PINNED SOURCE / ACCOUNT STORE INVENTORY ==="
PYTHONDONTWRITEBYTECODE=1 python3 -B tools/ci/audit_account_rollout_readiness.py
echo "ORUN_S6G_READ_ONLY_INVENTORY_COMPLETE"
echo "NOT A PRODUCTION ACTIVATION OR APPROVAL"
