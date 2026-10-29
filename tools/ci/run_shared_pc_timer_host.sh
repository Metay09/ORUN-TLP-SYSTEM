#!/usr/bin/env bash
# S4B isolated acceptance; generates NO installed units, runs NO services.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN S4B: PRIVATE CREDENTIAL + DISABLED TIMER ==="
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v \
  backend.tests.test_shared_pc_timer \
  backend.tests.test_shared_pc_mirror
echo "=== ORUN S3C LIVE HTTP CROSS-LANGUAGE REGRESSION ==="
PYTHONDONTWRITEBYTECODE=1 python3 tools/ci/test_shared_sync_real_http_e2e.py
echo "ORUN_S4B_HOST_PASS"
