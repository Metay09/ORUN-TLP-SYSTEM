#!/usr/bin/env bash
# ORUN password-first and unified observation API acceptance.
# Host-only, disposable SQLite and mock HTTP; no live services or APK build.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN ACCOUNT PASSWORD-FIRST HOST ==="
PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest -v \
  backend.tests.test_account_device_login \
  backend.tests.test_account_password_only
echo "ORUN_ACCOUNT_PASSWORD_BACKEND_HOST_PASS"
echo "=== ORUN ANDROID ACCOUNT TRANSPORT ==="
node --experimental-strip-types tools/ci/test_owner_account_client.ts
node --experimental-strip-types tools/ci/test_password_only_account_client.ts
echo "ORUN_ACCOUNT_PASSWORD_ANDROID_HOST_PASS"
echo "=== ORUN TS TYPECHECK ==="
(cd edge/app && npm run typecheck)
echo "ORUN_ACCOUNT_PASSWORD_FIRST_HOST_PASS"
echo "LIVE_BACKEND_CHANGE=NO"
echo "APK_BUILD=NO"
