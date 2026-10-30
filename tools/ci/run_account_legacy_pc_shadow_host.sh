#!/usr/bin/env bash
# ORUN S6H: pinned S4E-shaped synthetic PC credentials with new account mode.
# Temporary 0700 SQLite + 127.0.0.1 random HTTP port only.
# No production DB, service mutation, Tailscale change, APK or firmware flash.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== ORUN S6H OLD PC / NEW ACCOUNT SHADOW E2E ==="
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v \
  backend.tests.test_account_legacy_pc_cutover_shadow
echo "ORUN_S6H_LEGACY_PC_ACCOUNT_SHADOW_HOST_PASS"
echo "LIVE_RUNTIME_NOT_MODIFIED=YES"
echo "PRODUCTION_CUTOVER_APPROVED=NO"
