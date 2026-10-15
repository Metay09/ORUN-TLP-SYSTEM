#pragma once

#include <stddef.h>
#include <stdint.h>

namespace orun_tlp {

// SF5J2B1: dormant PERIODIC-only store-before-send coordinator.
// The port is a loop-owned adapter to reviewed ProductRecordStore,
// ProductSecureCrypto/SecurityStore and RadioManager; no production adapter
// or caller is enabled by this slice.
struct ProductSendIdentity {
  uint64_t incarnation = 0;
  uint32_t sequence = 0;
  bool valid() const { return incarnation != 0U && sequence != 0U; }
};

inline bool sameProductSendIdentity(const ProductSendIdentity& a,
                                   const ProductSendIdentity& b) {
  return a.incarnation == b.incarnation && a.sequence == b.sequence;
}

struct ProductSendPeriodic {
  ProductSendIdentity identity{};
  uint8_t plaintext[64]{};
};

struct ProductSendExact {
  ProductSendIdentity identity{};
  uint8_t bytes[100]{};
  uint8_t size = 0;
};

enum class ProductSendRead : uint8_t { kFound, kNone, kUnavailable };
enum class ProductSendSeal : uint8_t { kOk, kUnavailable, kFailed };
enum class ProductSendCache : uint8_t {
  kStarted, kAlreadyDurable, kUnavailable, kNoCapacity, kRejected
};

class ProductSendPort {
 public:
  virtual ~ProductSendPort() {}
  virtual bool securityReady() const = 0;
  virtual ProductSendRead oldestPeriodic(ProductSendPeriodic& out) = 0;
  virtual ProductSendRead cached(const ProductSendIdentity& identity,
                                 ProductSendExact& out) = 0;
  // Must use the already-reviewed SecurityStore-owned ProductSecureCrypto.
  // No caller-supplied counter, root, or test credential is permitted.
  virtual ProductSendSeal sealOnce(const ProductSendPeriodic& record,
                                   ProductSendExact& out) = 0;
  virtual ProductSendCache submitCache(const ProductSendExact& exact) = 0;
  virtual bool takeCacheResult(ProductSendIdentity& identity,
                               bool& durable) = 0;
  // Required even after reboot: key epoch alone cannot prove that a cached
  // object belongs to the current credential lifetime. Fail closed unless
  // the adapter can establish this binding from reviewed durable authority.
  virtual bool currentCredentialOwns(const ProductSendExact& exact) const = 0;
  // Effective time between normal committed PERIODIC reports, in milliseconds.
  // No hard-coded 30s..15min retry escalation may bypass this cadence.
  virtual uint32_t reportPeriodMs() const = 0;
  virtual bool radioReady() const = 0;
  virtual bool transmitExact(const uint8_t* bytes, size_t size) = 0;
};

class ProductSendOwner {
 public:
  enum class State : uint8_t { kIdle, kAwaitCache, kRetry, kWaitReport, kFault };
  struct Diagnostics {
    uint32_t cached_recovered = 0;
    uint32_t sealed = 0;
    uint32_t cache_committed = 0;
    uint32_t tx_started = 0;
    uint32_t tx_refused = 0;
    uint32_t blocked_security = 0;
    uint32_t failures = 0;
  };

  explicit ProductSendOwner(ProductSendPort& port) : port_(port) {}
  void poll(uint32_t now_ms);
  // Called only for a newly COMMITTED report period, not from a free-running
  // timer. During an outage this arms at most one exact-byte contact probe.
  void onReportCommitted();
  // Called only AFTER a separately authenticated custody ACK has been mapped
  // to this exact record AND its release has durably committed. No ACK parsing,
  // authorization or store mutation is performed by this dormant class.
  void onCustodyReleaseCommitted(const ProductSendIdentity& identity,
                                 uint32_t now_ms);
  State state() const { return state_; }
  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  bool loadDurable(const ProductSendExact* expected);
  void fail();
  void scheduleHalfRetry(uint32_t now_ms);

  ProductSendPort& port_;
  State state_ = State::kIdle;
  ProductSendIdentity selected_{};
  ProductSendExact exact_{};
  uint32_t next_retry_ms_ = 0;
  bool retry_scheduled_ = false;
  uint32_t next_scan_ms_ = 0;
  bool scan_scheduled_ = false;
  bool half_retry_consumed_ = false;
  static constexpr uint32_t kBacklogPaceMs = 15000U;
  Diagnostics diagnostics_{};
};

}  // namespace orun_tlp
