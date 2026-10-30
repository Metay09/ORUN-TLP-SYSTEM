#!/usr/bin/env bash
# ORUN S6I — isolated source bundle and account-mode launch smoke.
# Writes ONLY under unittest TemporaryDirectory; never stages under user's
# ORUN-SHARED-RUNTIME, never reads production DB or restarts services.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== S6I PINNED ACCOUNT SOURCE BUNDLE HOST ==="
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v \
  backend.tests.test_account_isolated_runtime_package
echo "ORUN_S6I_ISOLATED_RUNTIME_PACKAGE_HOST_PASS"
echo "LIVE_SERVICES_CHANGED=NO"
echo "LIVE_RUNTIME_CUTOVER=NOT_ATTEMPTED"
echo "PRODUCTION_APPROVAL=NO"
