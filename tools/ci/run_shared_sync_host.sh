#!/usr/bin/env bash
# ORUN shared-data backend S2: self-contained Python tests only.
# No running backend/service, tracker, Gateway, USB serial or production DB.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
python3 -m unittest discover -s backend/tests -p 'test_shared_sync.py' -v
