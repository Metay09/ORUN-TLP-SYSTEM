#include <assert.h>
#include <stdio.h>

#include "network_service.h"
#include "tlp_position_packet.h"

using namespace orun_tlp;

namespace {

void makePosition(uint64_t source, uint32_t sequence, uint8_t* bytes) {
  const tlp::PositionPacket packet{
      source, sequence, 1700000000, 410000000, 290000000, 12345, 175, 9, 7};
  assert(tlp::serializePositionPacket(packet, bytes, tlp::kPositionPacketSize));
}

}  // namespace

int main() {
  uint8_t packet[tlp::kPositionPacketSize];
  makePosition(0x0102030405060708ULL, 77, packet);

  NetworkService tracker;
  tracker.begin(0x1111222233334444ULL, NodeRole::kTracker);
  assert(tracker.role() == NodeRole::kTracker);
  assert(!tracker.relayForwardingEnabled());
  assert(tracker.receive(packet, sizeof(packet), -100, 4, 1000).kind ==
         NetworkEventKind::kIgnoredPosition);

  tracker.setRelayForwardingEnabled(true);
  assert(tracker.role() == NodeRole::kTracker);
  assert(tracker.relayForwardingEnabled());
  const auto queued = tracker.receive(packet, sizeof(packet), -100, 4, 1001);
  assert(queued.kind == NetworkEventKind::kRelayQueued);
  assert(tracker.queuedCount() == 1);

  tracker.setRelayForwardingEnabled(false);
  assert(tracker.role() == NodeRole::kTracker);
  assert(!tracker.relayForwardingEnabled());
  assert(tracker.queuedCount() == 0);

  NetworkService relay;
  relay.begin(0x5555666677778888ULL, NodeRole::kRelay);
  assert(relay.role() == NodeRole::kRelay);
  assert(relay.relayForwardingEnabled());

  relay.setRole(NodeRole::kBase);
  assert(relay.role() == NodeRole::kBase);
  assert(!relay.relayForwardingEnabled());

  puts("B4 independent NetworkService relay forwarding seam: PASS");
}
