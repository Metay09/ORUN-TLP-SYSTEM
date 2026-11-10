#!/usr/bin/env bash
# One read-only verification pass for PC Edge queue telemetry, its owner API
# and the Android application's UI parsing. No service, firmware, DB or APK
# is installed or modified by this script.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
echo "=== PC EDGE / BACKEND TESTLERI ==="
python3 -m unittest -v \
  backend.tests.test_edge_queue_status \
  backend.tests.test_gateway_listesi \
  backend.tests.test_gateway_izni_otomatik

echo "=== TELEFON UYGULAMASI ==="
cd "$ROOT/edge/app"
if [[ ! -d node_modules ]]; then
  echo "DUR: edge/app/node_modules yok. Bagimliliklari mevcut test ortamina kur."
  exit 1
fi
npm run typecheck
npm test
npm run build
echo "=== ORUN KUYRUK TELEMETRISI DOGRULAMA PASS ==="
