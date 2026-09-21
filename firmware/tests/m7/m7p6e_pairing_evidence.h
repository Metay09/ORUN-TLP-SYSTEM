#pragma once

#include <stdint.h>

namespace orun_tlp {
namespace m7p6e_test {

// Test-only evidence counters for the M7P6E fresh-pairing coexistence probe.
// These counters are sampled across the Bluefruit event-task -> loop-task
// boundary; they are not production authorization state.
struct PairingEvidenceCounters {
  uint32_t lesc = 0;
  uint32_t auth = 0;
  uint32_t auth_success = 0;
  uint32_t auth_failure = 0;
  uint32_t auth_bonded_success = 0;
  uint32_t auth_lesc_bonded_success = 0;
  uint32_t sec_update = 0;
  uint32_t encrypted_update = 0;
  uint32_t disconnects = 0;
};

inline bool counterAdvanced(uint32_t start, uint32_t current) {
  // Equality, rather than ordering/subtraction, keeps the observation valid
  // across uint32_t wrap. A full 2^32-event lap between two bounded samples is
  // outside the probe's possible runtime.
  return current != start;
}

inline bool pairingEvidenceRejected(const PairingEvidenceCounters& start,
                                    const PairingEvidenceCounters& current) {
  return counterAdvanced(start.auth_failure, current.auth_failure);
}

inline bool pairingEvidenceDisconnected(const PairingEvidenceCounters& start,
                                        const PairingEvidenceCounters& current) {
  return counterAdvanced(start.disconnects, current.disconnects);
}

inline bool pairingEvidenceComplete(const PairingEvidenceCounters& start,
                                    const PairingEvidenceCounters& current) {
  if (pairingEvidenceRejected(start, current) ||
      pairingEvidenceDisconnected(start, current)) {
    return false;
  }

  // A LESC DH-key request alone is not successful pairing. Require the same
  // bounded evidence window to contain successful LESC authentication that
  // SoftDevice says resulted in a bond, plus an encrypted Mode-1 security
  // update. This is pairing-completion evidence, not an independent proof that
  // InternalFS durably persisted the bond. Bluefruit's first-pairing event order may report the security
  // update before AUTH_STATUS, so completion intentionally requires both facts
  // without imposing the wrong order.
  return counterAdvanced(start.lesc, current.lesc) &&
         counterAdvanced(start.auth_success, current.auth_success) &&
         counterAdvanced(start.auth_lesc_bonded_success,
                         current.auth_lesc_bonded_success) &&
         counterAdvanced(start.encrypted_update, current.encrypted_update);
}

}  // namespace m7p6e_test
}  // namespace orun_tlp
