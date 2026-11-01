#!/usr/bin/env bash
# ORUN S6K: synthetic rollback probe acceptance.
# Uses only TemporaryDirectory and historical Git source; production state
# and services are never accessed by this test runner.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN S6K PINNED S4E BACKWARD COMPATIBILITY HOST ==="
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v \
  backend.tests.test_account_legacy_rollback_s6k
echo "ORUN_S6K_LEGACY_ROLLBACK_HOST_PASS"
echo "LIVE_SQLITE_ACCESSED=NO"
echo "LIVE_SERVICE_RESTART=NOT_ATTEMPTED"
echo "PRODUCTION_ROLLBACK_APPROVAL=NO"
