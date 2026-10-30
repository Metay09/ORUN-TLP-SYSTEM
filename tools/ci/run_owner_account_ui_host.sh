#!/usr/bin/env bash
# S6B Android account UI host gate. No backend, user accounts, APK, firmware,
# systemd services, Tailscale or real credentials touched.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN S6B OWNER LOGIN UI TS HOST TESTS ==="
node --experimental-strip-types tools/ci/test_owner_account_client.ts
node --experimental-strip-types tools/ci/test_owner_account_pilot_runtime.ts

if [ ! -x edge/app/node_modules/.bin/tsc ]; then
  echo "ORUN_ACCOUNT_UI_NOT_RUN=EXISTING_TYPESCRIPT_DEPENDENCIES_MISSING"
  exit 3
fi
echo "=== ORUN S6B ANDROID UI TYPESCRIPT TYPECHECK ==="
(cd edge/app && npm run typecheck)
echo "ORUN_OWNER_ACCOUNT_UI_HOST_PASS"
