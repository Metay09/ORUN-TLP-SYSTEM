#!/usr/bin/env bash
# S4A host gate. NO Android build, live service, live DB or physical device.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN PC EDGE SHARED MIRROR: REAL HTTP + SQLITE ==="
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v backend.tests.test_shared_pc_mirror
echo "=== S3C CROSS-LANGUAGE LIVE HTTP REGRESSION ==="
PYTHONDONTWRITEBYTECODE=1 python3 tools/ci/test_shared_sync_real_http_e2e.py
echo "ORUN_S4A_HOST_PASS"
