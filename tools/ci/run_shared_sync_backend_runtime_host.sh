#!/usr/bin/env bash
# S4C: new isolated Backend process, S4B timer and S3C TS integration tests.
# NO live systemd changes, APK, physical device or production DB writes.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN S4C: RESTARTABLE BACKEND + ENROLLED PC READ MIRROR ==="
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v \
  backend.tests.test_shared_sync_loopback_runtime \
  backend.tests.test_shared_pc_timer \
  backend.tests.test_shared_pc_mirror
echo "=== S3C NODE TYPESCRIPT <-> PYTHON AUTHENTICATED HTTP ==="
PYTHONDONTWRITEBYTECODE=1 python3 tools/ci/test_shared_sync_real_http_e2e.py
echo "ORUN_S4C_HOST_PASS"
