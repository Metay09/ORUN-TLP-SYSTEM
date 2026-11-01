#!/usr/bin/env bash
# ORUN S6J synthetic fixture acceptance; NO access to user's live databases.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN S6J PRIVATE SQLITE SHADOW REGRESSION ==="
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v \
  backend.tests.test_account_shadow_data_migration_s6j
echo "ORUN_S6J_SHADOW_MIGRATION_HOST_PASS"
echo "LIVE_SQLITE_BACKUP=NOT_ATTEMPTED"
echo "LIVE_SERVICE_CUTOVER=NOT_ATTEMPTED"
echo "PRODUCTION_APPROVAL=NO"
