#!/usr/bin/env bash
# SF5L7: ORUN Edge phone app core (TypeScript), host only.
# Unit tests (node --test) + typecheck. Needs Node >= 22 (node:sqlite,
# type stripping); installs edge/app dependencies with `npm ci` if missing.
set -euo pipefail
cd "$(dirname "$0")/../../edge/app"
if ! command -v node >/dev/null 2>&1; then
  echo "SF5L7 Edge app: SKIPPED (node not installed)"; exit 0
fi
major="$(node -p 'process.versions.node.split(".")[0]')"
if [ "$major" -lt 22 ]; then
  echo "SF5L7 Edge app: SKIPPED (node $major < 22)"; exit 0
fi
[ -d node_modules ] || npm ci --no-audit --no-fund
npm test --silent
npx tsc --noEmit
echo "SF5L7 Edge app core: unit tests + typecheck PASS (HOST ONLY)"
