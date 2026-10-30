#!/usr/bin/env bash
# ORUN S6A candidate: isolated account + historical phone enrollment gates.
# Only temp localhost/SQLite and Node mocks. No APK, flash, systemctl,
# Tailscale, real credentials, existing backend/PC-Edge runtime mutations.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN OWNER ACCOUNT LOGIN REAL HTTP TESTS ==="
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v \
  backend.tests.test_account_device_login \
  backend.tests.test_edge_enrollment \
  backend.tests.test_shared_local_pc_enroll \
  backend.tests.test_shared_sync_loopback_runtime \
  backend.tests.test_phone_owner_enrollment_http
echo "=== ORUN PHONE AND ACCOUNT TYPESCRIPT HOST TESTS ==="
node --experimental-strip-types tools/ci/test_phone_enrollment_client.ts
node --experimental-strip-types tools/ci/test_complete_phone_enrollment.ts
node --experimental-strip-types tools/ci/test_owner_account_client.ts
node --experimental-strip-types tools/ci/test_owner_account_pilot_runtime.ts
if [ -x edge/app/node_modules/.bin/tsc ]; then
  (cd edge/app && npm run typecheck)
  echo "ORUN_ACCOUNT_ANDROID_TYPESCRIPT_TYPECHECK_PASS"
else
  echo "ORUN_ACCOUNT_ANDROID_TYPESCRIPT_TYPECHECK_NOT_RUN=DEPENDENCIES_ABSENT"
fi
echo "ORUN_OWNER_ACCOUNT_LOGIN_HOST_PASS"
