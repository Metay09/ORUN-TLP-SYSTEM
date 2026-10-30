#!/usr/bin/env bash
# ORUN S6E: real Python HTTP + actual Node SQLite on temporary local state.
# Strictly no live service start/restart, Tailscale, APK install, real accounts,
# tracker/gateway activity or production data access.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN S6E CROSS-LANGUAGE ACCOUNT REAL HTTP SQLITE ==="
PYTHONDONTWRITEBYTECODE=1 python3 tools/ci/test_account_backend_sqlite_crosscheck.py
echo "=== ORUN S6E ACCOUNT SESSION RACE + EDGE SQLITE REGRESSIONS ==="
node --test --experimental-strip-types \
  edge/app/test/accountSharedMirror.test.ts \
  edge/app/test/offlineSync.test.ts
echo "=== ORUN S6E TYPE SCRIPT AND ACCOUNT CLIENT REGRESSION ==="
node --experimental-strip-types tools/ci/test_owner_account_client.ts
node --experimental-strip-types tools/ci/test_owner_account_pilot_runtime.ts
node --experimental-strip-types tools/ci/test_owner_account_sqlite_wiring.ts
if [ ! -x edge/app/node_modules/.bin/tsc ]; then
  echo "ORUN_ACCOUNT_REAL_BACKEND_E2E_TYPECHECK_NOT_RUN=LOCAL_DEPENDENCIES_ABSENT"
  exit 3
fi
(cd edge/app && npm run typecheck)
echo "ORUN_ACCOUNT_REAL_BACKEND_E2E_TYPESCRIPT_PASS"
echo "ORUN_ACCOUNT_REAL_BACKEND_E2E_HOST_PASS"
