#!/usr/bin/env bash
# KQ05 v2/type0x0A REAL Tracker SecurityStore V3 / 5-frame HMAC PC gate.
# Reuses SF5K3M production SecurityStore + ProductRecordStore host fixture.
# No real Flash, LoRa, Tracker release, History migration or device upload.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Independently reproduce the *test-only* V3 credential's HKDF-SHA256
# using the Python standard library. Never use this fixture as a device key.
PYTHONDONTWRITEBYTECODE=1 python3 - <<'PY'
from pathlib import Path
import hashlib
import hmac
import re

source = Path("firmware/tests/sf5/test_kq05_tracker_batch_security_verify_host.cpp").read_text()
match = re.search(r'kIndependentScopedKey\[\]\s*=\s*"([0-9a-f]+)"', source)
assert match is not None, "Tracker HKDF fixture absent"
expected = match.group(1)
assert len(expected) == 64, "Scoped key hex must be exactly 64 chars"

credential_id = bytes(range(3, 19))
root = bytes(range(0x90, 0xB0))
scope = (
    b"ORUN-TLP-V2-GW-CUSTODY-ACK-v1"
    + (0x0102030405060708).to_bytes(8, "big")
    + (7).to_bytes(4, "big")
    + (0x2300230023002301).to_bytes(8, "big")
    + (0).to_bytes(4, "big")
    + (1).to_bytes(4, "big")
)
assert len(scope) == 57, "Unexpected custody HKDF domain"
prk = hmac.new(credential_id, root, hashlib.sha256).digest()
derived = hmac.new(prk, scope + b"\x01", hashlib.sha256).hexdigest()
assert hmac.compare_digest(derived, expected), "Scoped test key disagrees with independent HKDF-SHA256"
print("KQ05 independent Python HKDF test-vector oracle: PASS")
PY

g++ -std=gnu++11 -O1 -g -Wall -Wextra -Werror \
  -Wno-conversion -Wno-sign-conversion \
  -DORUN_KQ05_BATCH_HMAC_HOST_TEST=1 \
  -DORUN_KQ05_TRACKER_BATCH_VERIFY_HOST_TEST=1 \
  -Ifirmware/include -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_kq05_tracker_batch_security_verify_host.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/src/kq05_tracker_batch_ack_verifier.cpp \
  firmware/src/kq05_gateway_batch_custody_crypto.cpp \
  firmware/src/kq05_gateway_batch_custody_ack.cpp \
  firmware/src/custody_ack_release_owner.cpp \
  firmware/src/custody_ack_product_bridge.cpp \
  firmware/src/custody_ack_verifier.cpp firmware/src/custody_ack.cpp \
  firmware/src/product_record_store.cpp firmware/src/product_region.cpp \
  firmware/src/observation_store.cpp \
  firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  firmware/src/product_period_builder.cpp \
  firmware/src/security_store.cpp \
  firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp \
  firmware/src/security_v3_format.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  firmware/src/security_format.cpp \
  firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -lcrypto -o "$tmp/tracker-batch-security"

"$tmp/tracker-batch-security"
echo "KQ05 TRACKER REAL SECURITYSTORE FIVE-RECORD BATCH HMAC: PC PASS"
echo "NO RECORD RELEASE, NO RADIO, NO DEVICE FLASH, NO OLD HISTORY ERASE"
