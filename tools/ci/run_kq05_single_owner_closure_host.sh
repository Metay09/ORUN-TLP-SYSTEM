#!/usr/bin/env bash
# KQ05 duplicate record-source closure gate, PC only.
# Verifies isolated Tracker single owner, portable fake Flash behavior,
# actual KQ05 preflight and unchanged default image build. NEVER flashes a RAK,
# changes systemd services, migrates partitions or touches the production DB.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
  echo "DUR: tracked source changes; use a clean verify clone." >&2
  exit 1
fi
echo "=== KQ05 SINGLE-OWNER SOURCE GUARD ==="
PYTHONDONTWRITEBYTECODE=1 python3 \
  firmware/tests/sf5/test_kq05_single_product_owner_source.py

# Fail fast on missing pinned dependencies, R2 drift or host regression,
# before running the two relatively expensive RAK4630 firmware builds.
echo "=== MANDATORY FULL LEGACY + PRODUCT HOST REGRESSION ==="
bash firmware/tests/run_host_tests.sh

echo "=== KQ05 FULL ISOLATED HOST / PRODUCT FLASH / LAB TARGET ==="
bash tools/ci/run_kq05_lab_preflight.sh

echo "=== UNCHANGED DEFAULT RAK4630 FIRMWARE BUILD ==="
(
  cd firmware
  pio run -e rak4630_orun
)

cat <<'EOF'
================================================
KQ05 ONE-OWNER PC INTEGRATION GATES: PASS
LEGACY FLASH NOT ERASED / NO DATABASE CHANGE
DEFAULT FIRMWARE + KQ05 LAB COMPILE: PASS
NO MERGE, NO DEVICE FLASH, NOT FIELD RELEASE
================================================
EOF
