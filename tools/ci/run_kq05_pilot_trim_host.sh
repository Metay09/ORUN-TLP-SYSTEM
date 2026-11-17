#!/usr/bin/env bash
# KQ05 pilot trim: full PC-only regression/build + actual linked-size proof.
# NEVER flashes a RAK, erases partitions, changes DB or deploys services.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

# This remains the strict, complete source / native / both-target closure.
bash tools/ci/run_kq05_single_owner_closure_host.sh

NM="$HOME/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-nm"
SIZE="$HOME/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-size"
test -x "$NM" && test -x "$SIZE" || {
  echo "STOP: pinned ARM nm/size tools not found." >&2
  exit 2
}
NORMAL="firmware/.pio/build/rak4630_orun/firmware.elf"
KQ05="firmware/.pio/build/rak4630_kq05_lab/firmware.elf"
test -s "$NORMAL" && test -s "$KQ05" || {
  echo "STOP: both freshly built target ELFs required." >&2
  exit 2
}

# Compile/link proof, NOT a source substring: all methods defined by the
# legacy pilot are unreachable and excluded by the linker in KQ05.
if ! "$NM" -C --defined-only "$NORMAL" |
    grep -F 'orun_tlp::TrackerProductTxPilot::' >/dev/null; then
  echo "STOP: normal ORUN legacy pilot missing; normal image changed." >&2
  exit 2
fi
if "$NM" -C --defined-only "$KQ05" |
    grep -F 'orun_tlp::TrackerProductTxPilot::' >/dev/null; then
  echo "STOP: KQ05 still links legacy TrackerProductTxPilot methods." >&2
  exit 2
fi
echo "LINK CHECK: normal pilot present; KQ05 pilot absent: PASS"

flash_size() {
  "$SIZE" -A "$1" |
    awk '$1 == ".text" || $1 == ".ARM.exidx" || $1 == ".data" {total += $2}
         END {printf "%.0f", total}'
}
NORMAL_BYTES="$(flash_size "$NORMAL")"
KQ05_BYTES="$(flash_size "$KQ05")"
[[ "$NORMAL_BYTES" =~ ^[0-9]+$ && "$KQ05_BYTES" =~ ^[0-9]+$ ]] || {
  echo "STOP: invalid ELF size accounting." >&2
  exit 2
}
# Pinned operator baseline from the previous experimental single-owner
# SHA 6c89c4597e98191835603a7747110e3b3b33c60d, same toolchain/targets.
BASELINE_NORMAL=463292
BASELINE_KQ05=469868
printf 'NORMAL Flash: baseline=%d new=%d delta=%+d B\n' \
  "$BASELINE_NORMAL" "$NORMAL_BYTES" "$((NORMAL_BYTES-BASELINE_NORMAL))"
printf 'KQ05 Flash: baseline=%d new=%d delta=%+d B\n' \
  "$BASELINE_KQ05" "$KQ05_BYTES" "$((KQ05_BYTES-BASELINE_KQ05))"

# The normal image must not unexpectedly grow due to a LAB-only trim.
if (( NORMAL_BYTES != BASELINE_NORMAL )); then
  echo "STOP: default ORUN image changed; review before accepting trim." >&2
  exit 2
fi
if (( KQ05_BYTES >= BASELINE_KQ05 )); then
  echo "NO SAVING: KQ05 legacy pilot removal does not reduce linked Flash." >&2
  exit 2
fi
echo "KQ05 PILOT TRIM: PASS (actual linked Flash saving)"
echo "NO DEVICE UPLOAD / NO FIELD RELEASE / NO MERGE"
