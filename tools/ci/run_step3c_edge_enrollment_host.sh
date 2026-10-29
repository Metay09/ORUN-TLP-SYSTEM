#!/usr/bin/env bash
# Step 3C enrollment prototype only. Genuine Ed25519, ephemeral test databases.
# No live server changes, USB, RF, Gateway credentials, phone installation.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
python3 -c 'import cryptography; print("cryptography:", cryptography.__version__)'
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s backend/tests -p 'test_edge_enrollment.py' -v
