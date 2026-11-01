#!/usr/bin/env bash
# ORUN S6C account-scoped SQLite mirror gate. Host-only, Node SQLite in memory.
# No backend/token/account creation, no Android build, no service/Tailscale change.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN S6C REAL SQLITE ACCOUNT MIRROR TESTS ==="
node --test --experimental-strip-types \
  edge/app/test/accountSharedMirror.test.ts \
  edge/app/test/offlineSync.test.ts
echo "=== ORUN ACCOUNT CLIENT REGRESSION ==="
node --experimental-strip-types tools/ci/test_owner_account_client.ts
node --experimental-strip-types tools/ci/test_owner_account_pilot_runtime.ts
if [ -x edge/app/node_modules/.bin/tsc ]; then
  echo "=== ORUN ANDROID STRICT TYPESCRIPT ==="
  (cd edge/app && npm run typecheck)
  echo "ORUN_ACCOUNT_SQLITE_MIRROR_TYPESCRIPT_PASS"
else
  echo "ORUN_ACCOUNT_SQLITE_MIRROR_TYPESCRIPT_NOT_RUN=DEPENDENCIES_ABSENT"
  exit 3
fi
echo "ORUN_ACCOUNT_SQLITE_MIRROR_HOST_PASS"
