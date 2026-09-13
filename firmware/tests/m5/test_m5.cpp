#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lora_airtime.h"
#include "network_service.h"
#include "node_role.h"
#include "packet_dedupe.h"
#include "radio_config.h"
#include "tlp_position_packet.h"
#include "tlp_relay_forward_packet.h"

using namespace orun_tlp;

void makePosition(uint64_t source, uint32_t sequence, uint8_t* bytes) {
  const tlp::PositionPacket packet{
      source, sequence, 1700000000, 410000000, 290000000, 12345, 175, 9, 7};
  assert(tlp::serializePositionPacket(packet, bytes, tlp::kPositionPacketSize));
}

void envelopeCodecAndValidation() {
  uint8_t original[tlp::kPositionPacketSize];
  makePosition(0x0102030405060708ULL, 42, original);
  tlp::RelayForwardPacket input{};
  input.relay_device_id = 0x8877665544332211ULL;
  input.ingress_rssi_dbm = -117;
  input.ingress_snr_db = -8;
  memcpy(input.original_packet, original, sizeof(original));
  uint8_t bytes[tlp::kRelayForwardPacketSize];
  assert(tlp::serializeRelayForwardPacket(input, bytes, sizeof(bytes)));
  assert(bytes[0] == 1 && bytes[1] == 3 && bytes[10] == 1 &&
         bytes[11] == 34 && bytes[12] == 0xFF && bytes[13] == 0x8B &&
         bytes[14] == 0xF8);
  tlp::RelayForwardPacket output{};
  tlp::PositionPacket position{};
  assert(tlp::deserializeRelayForwardPacket(bytes, sizeof(bytes), &output,
                                             &position) ==
         tlp::RelayDecodeStatus::kOk);
  assert(output.relay_device_id == input.relay_device_id &&
         output.ingress_rssi_dbm == -117 && output.ingress_snr_db == -8);
  assert(!memcmp(output.original_packet, original, sizeof(original)));
  assert(position.source_device_id == 0x0102030405060708ULL &&
         position.sequence_number == 42);

  assert(tlp::deserializeRelayForwardPacket(bytes, sizeof(bytes) - 1, &output,
                                             &position) ==
         tlp::RelayDecodeStatus::kLength);
  uint8_t malformed[tlp::kRelayForwardPacketSize];
  memcpy(malformed, bytes, sizeof(bytes));
  malformed[11] = 33;
  assert(tlp::deserializeRelayForwardPacket(malformed, sizeof(malformed),
                                             &output, &position) ==
         tlp::RelayDecodeStatus::kOriginalLength);
  memcpy(malformed, bytes, sizeof(bytes)); malformed[0] = 2;
  assert(tlp::deserializeRelayForwardPacket(malformed, sizeof(malformed),
                                             &output, &position) ==
         tlp::RelayDecodeStatus::kVersion);
  memcpy(malformed, bytes, sizeof(bytes)); malformed[10] = 2;
  assert(tlp::deserializeRelayForwardPacket(malformed, sizeof(malformed),
                                             &output, &position) ==
         tlp::RelayDecodeStatus::kHopCount);
  memcpy(malformed, bytes, sizeof(bytes));
  malformed[tlp::kRelayForwardHeaderSize + 1] = tlp::kPacketTypeRelayForward;
  assert(tlp::deserializeRelayForwardPacket(malformed, sizeof(malformed),
                                             &output, &position) ==
         tlp::RelayDecodeStatus::kNestedRelay);
  memcpy(malformed, bytes, sizeof(bytes));
  malformed[tlp::kRelayForwardHeaderSize] = 2;
  assert(tlp::deserializeRelayForwardPacket(malformed, sizeof(malformed),
                                             &output, &position) ==
         tlp::RelayDecodeStatus::kInnerVersion);
}

void dedupeAndQueue() {
  NetworkService relay;
  relay.begin(0xAABBCCDD00112233ULL, NodeRole::kRelay);
  uint8_t packet[tlp::kPositionPacketSize];
  makePosition(1, 10, packet);
  auto event = relay.receive(packet, sizeof(packet), -100, 4, 1000);
  assert(event.kind == NetworkEventKind::kRelayQueued && relay.queuedCount() == 1);
  assert(relay.receive(packet, sizeof(packet), -101, 3, 1001).kind ==
         NetworkEventKind::kRelayDuplicate);
  makePosition(1, 11, packet);
  assert(relay.receive(packet, sizeof(packet), -100, 4, 1002).kind ==
         NetworkEventKind::kRelayQueued);
  makePosition(2, 10, packet);
  assert(relay.receive(packet, sizeof(packet), -100, 4, 1003).kind ==
         NetworkEventKind::kRelayQueued);
  makePosition(3, 10, packet);
  assert(relay.receive(packet, sizeof(packet), -100, 4, 1004).kind ==
         NetworkEventKind::kRelayQueued);
  makePosition(4, 10, packet);
  assert(relay.receive(packet, sizeof(packet), -100, 4, 1005).kind ==
         NetworkEventKind::kRelayQueueDrop);
  assert(relay.queuedCount() == relay_config::kForwardQueueSize);

  tlp::RelayForwardPacket outgoing{};
  assert(relay.takeDueForward(1000 + relay_config::kMaximumDelayMs, &outgoing));
  relay.onForwardTxStarted();
  assert(!relay.takeDueForward(1000 + relay_config::kMaximumDelayMs, &outgoing));
  relay.onForwardTxResult(true);
  assert(relay.takeDueForward(1005 + relay_config::kMaximumDelayMs, &outgoing));
  relay.onForwardTxStarted();
  relay.onForwardTxResult(false);
  assert(relay.relayDiagnostics().forwards_attempted == 2 &&
         relay.relayDiagnostics().forwards_completed == 1 &&
         relay.relayDiagnostics().forward_failures == 1);

  PacketDedupeCache<3> cache;
  assert(!cache.observe({1, 1}) && !cache.observe({1, 2}) &&
         !cache.observe({1, 3}) && cache.observe({1, 1}));
  assert(!cache.observe({1, 4}));
  assert(!cache.contains({1, 1}) && cache.contains({1, 2}));
}

void basePathSemantics(bool relay_first) {
  uint8_t direct[tlp::kPositionPacketSize];
  makePosition(0x1234, 99, direct);
  tlp::RelayForwardPacket envelope{};
  envelope.relay_device_id = 0x5678;
  envelope.ingress_rssi_dbm = -110;
  envelope.ingress_snr_db = -3;
  memcpy(envelope.original_packet, direct, sizeof(direct));
  uint8_t forwarded[tlp::kRelayForwardPacketSize];
  assert(tlp::serializeRelayForwardPacket(envelope, forwarded, sizeof(forwarded)));
  NetworkService base;
  base.begin(0x9999, NodeRole::kBase);
  const auto first = relay_first
      ? base.receive(forwarded, sizeof(forwarded), -95, 6, 0)
      : base.receive(direct, sizeof(direct), -80, 9, 0);
  const auto second = relay_first
      ? base.receive(direct, sizeof(direct), -80, 9, 1)
      : base.receive(forwarded, sizeof(forwarded), -95, 6, 1);
  assert(first.kind == NetworkEventKind::kBaseNew);
  assert(second.kind == NetworkEventKind::kBaseDuplicate);
  assert(base.baseDiagnostics().new_application_packets == 1 &&
         base.baseDiagnostics().duplicates == 1 &&
         base.baseDiagnostics().direct_observations == 1 &&
         base.baseDiagnostics().relay_observations == 1);
}

void timingRoleNestedAndAirtime() {
  const uint64_t source = 0x1020304050607080ULL;
  const uint32_t sequence = 0xAABBCCDD;
  const uint64_t relay = 0x1111222233334444ULL;
  const uint32_t delay = deterministicRelayDelay(source, sequence, relay);
  assert(delay >= relay_config::kMinimumDelayMs &&
         delay <= relay_config::kMaximumDelayMs);
  assert(delay == deterministicRelayDelay(source, sequence, relay));
  bool differentiated = false;
  for (uint64_t candidate = relay + 1; candidate < relay + 20; ++candidate)
    differentiated |= delay != deterministicRelayDelay(source, sequence, candidate);
  assert(differentiated);

  NetworkService service;
  service.begin(relay, NodeRole::kRelay);
  uint8_t direct[tlp::kPositionPacketSize]; makePosition(source, sequence, direct);
  const uint32_t now = UINT32_MAX - 100;
  auto event = service.receive(direct, sizeof(direct), -99, 1, now);
  const uint32_t due = now + event.relay_delay_ms;
  tlp::RelayForwardPacket outgoing{};
  assert(!service.takeDueForward(due - 1, &outgoing));
  assert(service.takeDueForward(due, &outgoing));

  uint8_t relayed[tlp::kRelayForwardPacketSize];
  assert(tlp::serializeRelayForwardPacket(outgoing, relayed, sizeof(relayed)));
  assert(service.receive(relayed, sizeof(relayed), -90, 5, due).kind ==
         NetworkEventKind::kRelayNestedRejected);

  assert(parseRoleCommand("role tracker", 12) == RoleCommand::kTracker);
  assert(parseRoleCommand("ROLE RELAY", 10) == RoleCommand::kRelay);
  assert(parseRoleCommand("ROLE BASE", 9) == RoleCommand::kBase);
  assert(parseRoleCommand("ROLE?", 5) == RoleCommand::kQuery);
  RoleController roles;
  assert(roles.role() == NodeRole::kBase && roles.automatic());
  assert(!roles.updateAutomatic(false, true));
  assert(roles.updateAutomatic(true, true) && roles.role() == NodeRole::kTracker);
  assert(roles.applyOverride(NodeRole::kRelay) && !roles.automatic());
  assert(!roles.updateAutomatic(true, false) && roles.role() == NodeRole::kRelay);

  assert(estimateLoraAirtimeUs(34, 11, 125, 1, 8, true, true) == 987136);
  assert(estimateLoraAirtimeUs(49, 11, 125, 1, 8, true, true) == 1232896);
  service.onForwardTxStarted();
  service.onForwardTxResult(true);
  assert(service.baseDiagnostics().new_application_packets == 0);
}

int main() {
  envelopeCodecAndValidation();
  dedupeAndQueue();
  basePathSemantics(false);
  basePathSemantics(true);
  timingRoleNestedAndAirtime();
  puts("M5 relay, dedupe, role and timing checks: PASS");
}
