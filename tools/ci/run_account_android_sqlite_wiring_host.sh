#!/usr/bin/env bash
# ORUN S6D — real Node SQLite + EdgeCore serialization + account UI preflight.
# No APK generation/install, firmware flashing, live services, tokens, or
# production database writes. Does not run host on live ORUN SQLite.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN ACCOUNT ANDROID SQLITE EXCLUSIVE HOST TESTS ==="
node --test --experimental-strip-types \
  edge/app/test/accountSharedMirror.test.ts \
  edge/app/test/offlineSync.test.ts
echo "=== ORUN ACCOUNT ANDROID SQLITE RUNTIME HOST TEST ==="
node --experimental-strip-types tools/ci/test_owner_account_sqlite_wiring.ts
echo "=== ORUN ACCOUNT LOGIN REGRESSION ==="
node --experimental-strip-types tools/ci/test_owner_account_client.ts
node --experimental-strip-types tools/ci/test_owner_account_pilot_runtime.ts
if [ ! -x edge/app/node_modules/.bin/tsc ]; then
  echo "ORUN_ACCOUNT_ANDROID_SQLITE_WIRING_TYPECHECK_NOT_RUN=DEPENDENCIES_ABSENT"
  exit 3
fi
echo "=== ORUN ACCOUNT ANDROID STRICT TYPESCRIPT ==="
(cd edge/app && npm run typecheck)
echo "ORUN_ACCOUNT_ANDROID_SQLITE_WIRING_TYPESCRIPT_PASS"
echo "ORUN_ACCOUNT_ANDROID_SQLITE_WIRING_HOST_PASS"
