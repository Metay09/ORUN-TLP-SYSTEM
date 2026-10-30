#!/usr/bin/env bash
# ORUN phone enrollment integration gate. Host-only, tmp SQLite + loopback.
# No app installation, live ORUN services, Tailscale, Gateway or firmware mutation.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN PHONE OWNER ENROLLMENT HOST TESTS ==="
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v \
  backend.tests.test_edge_enrollment \
  backend.tests.test_shared_local_pc_enroll \
  backend.tests.test_shared_sync_loopback_runtime \
  backend.tests.test_phone_owner_enrollment_http
echo "=== ORUN PHONE ENROLLMENT TYPESCRIPT CLIENT ==="
node --experimental-strip-types tools/ci/test_phone_enrollment_client.ts
node --experimental-strip-types tools/ci/test_complete_phone_enrollment.ts
# No dependency installs, Gradle tasks, APK builds or firmware flashes.
# If the existing worktree already has npm dependencies, also check the
# integrated Android UI under the app's strict TypeScript configuration.
if [ -x edge/app/node_modules/.bin/tsc ]; then
  echo "=== ORUN ANDROID UI TYPESCRIPT TYPECHECK ==="
  (cd edge/app && npm run typecheck)
  echo "ORUN_ANDROID_UI_TYPESCRIPT_TYPECHECK_PASS"
else
  echo "ORUN_ANDROID_UI_TYPESCRIPT_TYPECHECK_NOT_RUN=LOCAL_DEPENDENCIES_ABSENT"
fi
echo "ORUN_PHONE_OWNER_ENROLLMENT_HOST_PASS"
