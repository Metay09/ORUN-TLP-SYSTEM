#include <assert.h>
#include <stdint.h>

#include "m7p6e_pairing_evidence.h"

int main() {
  using namespace orun_tlp::m7p6e_test;

  PairingEvidenceCounters start{};
  PairingEvidenceCounters now = start;

  // LESC work beginning is necessary but not sufficient.
  ++now.lesc;
  assert(!pairingEvidenceComplete(start, now));

  // Successful authentication without a resulting bond is still incomplete
  // for the stock bond-requesting Bluefruit path exercised by this probe.
  ++now.auth;
  ++now.auth_success;
  assert(!pairingEvidenceComplete(start, now));

  ++now.auth_bonded_success;

  // Even with encryption, a successful bond that was not itself reported as
  // LESC is not enough if a DH-key event happened earlier in the window.
  ++now.sec_update;
  ++now.encrypted_update;
  assert(!pairingEvidenceComplete(start, now));

  // The completed authentication itself must be LESC + bonded.
  ++now.auth_lesc_bonded_success;
  assert(pairingEvidenceComplete(start, now));
  assert(!pairingEvidenceRejected(start, now));
  assert(!pairingEvidenceDisconnected(start, now));

  // A rejected pairing must never be promotable to PASS even if other
  // counters also moved.
  PairingEvidenceCounters rejected = now;
  ++rejected.auth;
  ++rejected.auth_failure;
  assert(pairingEvidenceRejected(start, rejected));
  assert(!pairingEvidenceComplete(start, rejected));

  // Likewise for a connection loss during the evidence window.
  PairingEvidenceCounters disconnected = now;
  ++disconnected.disconnects;
  assert(pairingEvidenceDisconnected(start, disconnected));
  assert(!pairingEvidenceComplete(start, disconnected));

  // Security update alone is not evidence of a fresh pairing. This prevents a
  // bonded reconnect/security refresh from being mistaken for the LESC test.
  PairingEvidenceCounters reconnect = start;
  ++reconnect.sec_update;
  ++reconnect.encrypted_update;
  assert(!pairingEvidenceComplete(start, reconnect));

  // Counter wrap must still count as a new event.
  PairingEvidenceCounters wrap_start{};
  wrap_start.lesc = UINT32_MAX;
  wrap_start.auth = UINT32_MAX;
  wrap_start.auth_success = UINT32_MAX;
  wrap_start.auth_bonded_success = UINT32_MAX;
  wrap_start.auth_lesc_bonded_success = UINT32_MAX;
  wrap_start.sec_update = UINT32_MAX;
  wrap_start.encrypted_update = UINT32_MAX;
  wrap_start.auth_failure = 7;
  wrap_start.disconnects = 9;

  PairingEvidenceCounters wrap_now = wrap_start;
  wrap_now.lesc = 0;
  wrap_now.auth = 0;
  wrap_now.auth_success = 0;
  wrap_now.auth_bonded_success = 0;
  wrap_now.auth_lesc_bonded_success = 0;
  wrap_now.sec_update = 0;
  wrap_now.encrypted_update = 0;
  assert(pairingEvidenceComplete(wrap_start, wrap_now));

  return 0;
}
