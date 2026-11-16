#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tlp_v2_relay_forward.h"

namespace orun_tlp {

// SF5F candidate: RAM-only bounded forwarding admission, deliberately NOT
// wired into RadioManager. It does not grant custody, alter any ciphertext,
// schedule RX rendezvous or infer delivery from TX_DONE.
class V2RelayForwardQueue {
 public:
  static constexpr uint8_t kCapacity = 4U;

  enum class Offer : uint8_t {
    kQueued,
    kDuplicate,
    kFull,
    kInvalid,
  };

  // Due times are selected by an external airtime/rendezvous owner. The
  // queue itself deliberately invents no legal transmit budget or ACK timer.
  Offer offer(const uint8_t* inner, size_t length, int16_t ingress_rssi_dbm,
              int8_t ingress_snr_db, uint32_t due_ms);

  // Pure read: permits radio admission checks before the caller starts TX.
  // Among due entries, a Gateway custody ACK takes precedence over telemetry.
  bool peekDue(uint32_t now_ms, tlp::V2RelayForward& out) const;

  // Remove only the exact previously-selected packet AFTER RF admission.
  // This is not delivery proof. On loss the Tracker retains its own record.
  bool consume(const tlp::V2RelayForward& selected);

  uint8_t queued() const { return queued_; }
  void clear();  // Role/service transition; never mutates durable records.

 private:
  struct Entry {
    bool used = false;
    uint32_t due_ms = 0;
    tlp::V2RelayForward packet{};
  };

  Entry entries_[kCapacity]{};
  uint8_t queued_ = 0U;
};

}  // namespace orun_tlp
