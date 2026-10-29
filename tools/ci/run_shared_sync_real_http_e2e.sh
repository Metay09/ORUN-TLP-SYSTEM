#!/usr/bin/env bash
# ORUN S3C: real HTTP, real Ed25519, actual TypeScript client interop.
# Only loopback-bound ephemeral SQLite and in-memory phone mirror fixtures.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo '=== ORUN S3C LIVE LOOPBACK HTTP E2E ==='
PYTHONDONTWRITEBYTECODE=1 python3 tools/ci/test_shared_sync_real_http_e2e.py
