// SF5F: strictly RAM-only v2 relay queue. No RadioManager or flash wiring.
#include <assert.h>
#include <string.h>

#include "custody_ack.h"
#include "tlp_v2_product_secure.h"
#include "tlp_v2_relay_forward.h"
#include "v2_relay_forward_queue.h"

using namespace orun_tlp;

namespace {

void makeProduct(uint8_t (&out)[tlp::kPeriodicFrameSize], uint64_t counter) {
  tlp::ProductSecureFrame frame{};
  frame.app_family = tlp::kProductAppFamilyPeriodic;
  frame.path_flags =
      tlp::kProductPathFlagRelayAllowed |
      tlp::kProductPathFlagCustodyRequested;
  frame.ciphertext_len = tlp::kPeriodicPlaintextSize;
  frame.device_id = 12345U;
  frame.key_epoch = 7U;
  frame.security_counter = counter;
  assert(tlp::encodeProductSecureFrame(frame, out, sizeof(out)) ==
         tlp::ProductSecureCodecResult::kOk);
}

void makeAck(uint8_t (&out)[custody_ack::kWireSize]) {
  memset(out, 0, sizeof(out));
  out[0] = custody_ack::kVersion;
  out[1] = custody_ack::kType;
  out[2] = custody_ack::kRelayAllowed;
  out[3] = tlp::kPeriodicFrameSize;
  out[11] = 1U;  // tracker
  out[19] = 2U;  // gateway
  out[23] = 7U;  // key epoch
  out[31] = 1U;  // grant generation
  custody_ack::Header header{};
  assert(custody_ack::decodeHeader(out, sizeof(out), header));
}

}  // namespace

int main() {
  V2RelayForwardQueue queue{};
  uint8_t product[tlp::kPeriodicFrameSize]{};
  uint8_t receipt[custody_ack::kWireSize]{};
  makeProduct(product, 1U);
  makeAck(receipt);

  assert(queue.offer(product, sizeof(product), -105, -4, 100U) ==
         V2RelayForwardQueue::Offer::kQueued);
  assert(queue.offer(product, sizeof(product), -105, -4, 200U) ==
         V2RelayForwardQueue::Offer::kDuplicate);
  assert(queue.queued() == 1U);

  // No packet is lost merely because RadioManager currently cannot send.
  tlp::V2RelayForward selected{};
  assert(!queue.peekDue(99U, selected));
  assert(queue.queued() == 1U);

  // Once both are due, the exact one-object ACK preempts telemetry. This
  // does not grant custody or substitute for sleepy-tracker rendezvous.
  assert(queue.offer(receipt, sizeof(receipt), -120, 2, 150U) ==
         V2RelayForwardQueue::Offer::kQueued);
  assert(queue.peekDue(151U, selected));
  assert(selected.inner_length == sizeof(receipt));
  assert(memcmp(selected.inner, receipt, sizeof(receipt)) == 0);
  assert(queue.queued() == 2U);  // peek is truly read-only

  // Wire encoding preserves exact protected bytes. The relay ID is assigned
  // by the separate radio owner, not taken from an untrusted inner header.
  selected.relay_device_id = 900U;
  uint8_t wire[tlp::kV2RelayForwardMaxSize]{};
  size_t length = 0U;
  assert(tlp::encodeV2RelayForward(selected, wire, sizeof(wire), length));
  tlp::V2RelayForward decoded{};
  assert(tlp::decodeV2RelayForward(wire, length, decoded) ==
         tlp::V2RelayCodecStatus::kOk);
  assert(memcmp(decoded.inner, receipt, sizeof(receipt)) == 0);

  assert(queue.consume(selected));
  assert(queue.queued() == 1U);
  assert(!queue.consume(selected));
  assert(queue.peekDue(151U, selected));
  assert(selected.inner_length == sizeof(product));
  assert(memcmp(selected.inner, product, sizeof(product)) == 0);
  assert(queue.consume(selected));
  assert(queue.queued() == 0U);

  // Different protected frames are distinct; no implicit ACK range/bitmap.
  for (uint64_t counter = 1U; counter <=
       V2RelayForwardQueue::kCapacity; ++counter) {
    makeProduct(product, counter);
    assert(queue.offer(product, sizeof(product), -1, 1, 200U) ==
           V2RelayForwardQueue::Offer::kQueued);
  }
  makeProduct(product, 5U);
  assert(queue.offer(product, sizeof(product), -1, 1, 200U) ==
         V2RelayForwardQueue::Offer::kFull);
  assert(queue.queued() == V2RelayForwardQueue::kCapacity);

  // Invalid nested/relay-forbidden input never evicts queued work.
  uint8_t nested[tlp::kPeriodicFrameSize]{};
  nested[0] = 2U; nested[1] = 2U;
  assert(queue.offer(nested, sizeof(nested), 0, 0, 200U) ==
         V2RelayForwardQueue::Offer::kInvalid);
  makeProduct(product, 6U);
  product[4] = tlp::kProductPathFlagCustodyRequested;
  assert(queue.offer(product, sizeof(product), 0, 0, 200U) ==
         V2RelayForwardQueue::Offer::kInvalid);
  assert(queue.queued() == V2RelayForwardQueue::kCapacity);

  // Transition can discard only relay RAM copies, never a Tracker record.
  queue.clear();
  assert(queue.queued() == 0U);
  assert(!queue.peekDue(200U, selected));

  // Deadline calculations are 32-bit monotonic rollover safe.
  makeProduct(product, 99U);
  assert(queue.offer(product, sizeof(product), -8, 3, 2U) ==
         V2RelayForwardQueue::Offer::kQueued);
  assert(!queue.peekDue(UINT32_MAX - 4U, selected));
  assert(queue.peekDue(3U, selected));
  assert(queue.consume(selected));

  return 0;
}
