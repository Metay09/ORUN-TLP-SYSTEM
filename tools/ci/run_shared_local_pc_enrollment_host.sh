#!/usr/bin/env bash
# S4D isolated local owner PC enrollment + real backend HTTP + PC mirror.
# No APK, running ORUN service, production DB, USB or actual provision.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo '=== ORUN S4D OWNER-APPROVED LOCAL PC EDGE ==='
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v \
  backend.tests.test_shared_local_pc_enroll \
  backend.tests.test_shared_sync_loopback_runtime \
  backend.tests.test_shared_pc_timer \
  backend.tests.test_shared_pc_mirror
echo '=== S3C REAL HTTP ANDROID TYPESCRIPT REGRESSION ==='
PYTHONDONTWRITEBYTECODE=1 python3 tools/ci/test_shared_sync_real_http_e2e.py
echo 'ORUN_S4D_HOST_PASS'
