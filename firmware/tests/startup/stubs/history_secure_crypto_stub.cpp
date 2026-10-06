// STARTUP HOST TEST ONLY.
//
// The production SF3 runtime is linked into the startup graph, but the host
// startup binary has no nRF52840 CC310 implementation. Keep crypto unavailable
// in this legacy startup harness; real HistorySecureCrypto is compiled and
// exercised by the RAK4630 build/target path.
//
// These stubs exist only to preserve the existing startup-state-machine tests
// while linking the actual HistoryStoreForwardRuntime implementation.
#include "history_secure_crypto.h"

namespace orun_tlp {

HistorySecureCryptoResult HistorySecureCrypto::protectNextObservation(
    uint64_t,
    uint8_t,
    const tlp::HistoryObservationPlaintext&,
    tlp::HistorySecurePacket&) {
  return HistorySecureCryptoResult::kUnavailable;
}

HistorySecureCryptoResult HistorySecureCrypto::openBackendDurableReceipt(
    const uint8_t*, size_t, uint64_t,
    AuthenticatedBackendDurableReceipt&) {
  return HistorySecureCryptoResult::kUnavailable;
}

}  // namespace orun_tlp
