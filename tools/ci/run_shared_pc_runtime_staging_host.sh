#!/usr/bin/env bash
# S4E: test actual isolated runtime and PC read-only mirror from pinned snapshot.
# No live ORUN services touched; never calls systemctl or builds an APK.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN S4E PINNED RUNTIME AND STAGING SAFETY ==="
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v \
  backend.tests.test_shared_pc_runtime_staging
echo "ORUN_S4E_STAGING_HOST_PASS"
