#!/usr/bin/env bash
# ORUN S6L host tests. All databases in temporary synthetic fixtures.
# Does not inspect the owner's ~/ORUN-SHARED-RUNTIME or change services.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN S6L DISPOSABLE ROLLBACK RESTORE HOST ==="
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v \
  backend.tests.test_account_disposable_rollback_restore_s6l
echo "ORUN_S6L_DISPOSABLE_ROLLBACK_HOST_PASS"
echo "LIVE_DATABASE_ACCESSED=NO"
echo "LIVE_SERVICE_RESTART=NOT_ATTEMPTED"
echo "CONSISTENT_PRODUCTION_CHECKPOINT=NOT_PROVEN"
echo "PRODUCTION_APPROVAL=NO"
