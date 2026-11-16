// SF5F: transport-only V2_RELAY_FORWARD codec. Synthetic protected bytes
// are structurally valid, NOT authenticated. No RF, crypto keys or flash.
#include <assert.h>
#include <string.h>

#include "tlp_v2_relay_forward.h"
#include "tlp_v2_delegated_secure_app.h"
#include "custody_ack.h"

using namespace orun_tlp;
namespace tlp = orun_tlp::tlp;

namespace {

void product(uint8_t (&bytes)[tlp::kPeriodicFrameSize],
             uint8_t path_flags = tlp::kProductPathFlagRelayAllowed |
                                  tlp::kProductPathFlagCustodyRequested) {
  tlp::ProductSecureFrame frame{};
  frame.app_family = tlp::kProductAppFamilyPeriodic;
  frame.path_flags = path_flags;
  frame.ciphertext_len = tlp::kPeriodicPlaintextSize;
  frame.device_id = UINT64_C(0x0102030405060708);
  frame.key_epoch = 2U;
  frame.security_counter = 3U;
  assert(tlp::encodeProductSecureFrame(frame, bytes, sizeof(bytes)) ==
         tlp::ProductSecureCodecResult::kOk);
}

void ack(uint8_t (&bytes)[custody_ack::kWireSize]) {
  memset(bytes, 0, sizeof(bytes));
  bytes[0] = custody_ack::kVersion;
  bytes[1] = custody_ack::kType;
  bytes[2] = custody_ack::kRelayAllowed;
  bytes[3] = tlp::kPeriodicFrameSize;
  bytes[11] = 8U;  // tracker device
  bytes[19] = 9U;  // gateway device
  bytes[23] = 2U;  // key epoch
  bytes[31] = 1U;  // grant generation
  custody_ack::Header h{};
  assert(custody_ack::decodeHeader(bytes, sizeof(bytes), h));
  assert(h.relay_allowed);
}

void delegated(uint8_t (&bytes)[tlp::kDelegatedSecureMinPacketSize]) {
  tlp::DelegatedSecureAppPacket packet{};
  packet.security_context = tlp::kSecurityContextDelegatedGw2d;
  packet.app_family = tlp::kDelegatedAppFamilyCommand;
  packet.path_flags = tlp::kDelegatedPathFlagRelayAllowed;
  packet.origin_device_id = 1U;
  packet.target_device_id = 2U;
  packet.key_epoch = 2U;
  packet.security_counter = 3U;
  packet.gateway_grant_generation = 1U;
  assert(tlp::serializeDelegatedSecureAppPacket(packet, bytes, sizeof(bytes)));
}

void roundTrip(const uint8_t* inner, size_t length) {
  tlp::V2RelayForward original{};
  original.relay_device_id = UINT64_C(0x1122334455667788);
  original.ingress_rssi_dbm = -110;
  original.ingress_snr_db = -6;
  original.inner_length = static_cast<uint8_t>(length);
  memcpy(original.inner, inner, length);

  uint8_t wire[tlp::kV2RelayForwardMaxSize]{};
  size_t size = 0U;
  assert(tlp::encodeV2RelayForward(original, wire, sizeof(wire), size));
  assert(size == tlp::kV2RelayForwardHeaderSize + length);
  assert(wire[0] == 2U && wire[1] == 2U && wire[2] == 1U);
  assert(wire[3] == length && wire[4] == 0x11U && wire[11] == 0x88U);
  assert(wire[12] == 0xFFU && wire[13] == 0x92U);
  assert(wire[14] == 0xFAU && wire[15] == 0U);
  assert(memcmp(wire + tlp::kV2RelayForwardHeaderSize, inner, length) == 0);

  tlp::V2RelayForward decoded{};
  assert(tlp::decodeV2RelayForward(wire, size, decoded) ==
         tlp::V2RelayCodecStatus::kOk);
  assert(decoded.relay_device_id == original.relay_device_id);
  assert(decoded.ingress_rssi_dbm == -110);
  assert(decoded.ingress_snr_db == -6);
  assert(decoded.inner_length == length);
  assert(memcmp(decoded.inner, inner, length) == 0);

  // Decode rejections cannot corrupt a previous valid output.
  const tlp::V2RelayForward saved = decoded;
  const auto rejects = [&](size_t n) {
    assert(tlp::decodeV2RelayForward(wire, n, decoded) !=
           tlp::V2RelayCodecStatus::kOk);
    assert(saved.relay_device_id == decoded.relay_device_id);
    assert(saved.ingress_rssi_dbm == decoded.ingress_rssi_dbm);
    assert(saved.ingress_snr_db == decoded.ingress_snr_db);
    assert(saved.inner_length == decoded.inner_length);
    assert(memcmp(saved.inner, decoded.inner, sizeof(saved.inner)) == 0);
  };
  rejects(size - 1U);
  wire[2] = 2U; rejects(size); wire[2] = 1U;
  wire[15] = 1U; rejects(size); wire[15] = 0U;
  wire[3] = static_cast<uint8_t>(length - 1U); rejects(size);
  wire[3] = static_cast<uint8_t>(length);
  wire[0] = 1U; rejects(size); wire[0] = 2U;
  wire[4] = wire[5] = wire[6] = wire[7] = 0U;
  wire[8] = wire[9] = wire[10] = wire[11] = 0U;
  rejects(size);

  uint8_t too_short[1]{0xA5U};
  size_t unchanged = 99U;
  assert(!tlp::encodeV2RelayForward(original, too_short,
                                   sizeof(too_short), unchanged));
  assert(unchanged == 99U && too_short[0] == 0xA5U);
}

}  // namespace

int main() {
  uint8_t periodic[tlp::kPeriodicFrameSize]{};
  product(periodic);
  roundTrip(periodic, sizeof(periodic));

  uint8_t receipt[custody_ack::kWireSize]{};
  ack(receipt);
  roundTrip(receipt, sizeof(receipt));

  uint8_t delegated_frame[tlp::kDelegatedSecureMinPacketSize]{};
  delegated(delegated_frame);
  roundTrip(delegated_frame, sizeof(delegated_frame));

  // A direct-only protected object or ACK may NOT be silently wrapped.
  periodic[4] &= static_cast<uint8_t>(~tlp::kProductPathFlagRelayAllowed);
  assert(tlp::validateV2RelayInner(periodic, sizeof(periodic)) ==
         tlp::V2RelayCodecStatus::kRelayNotAllowed);
  product(periodic);
  periodic[7] = 1U;  // corrupt reserved frame byte
  assert(tlp::validateV2RelayInner(periodic, sizeof(periodic)) ==
         tlp::V2RelayCodecStatus::kInnerInvalid);
  receipt[2] = 0U;
  assert(tlp::validateV2RelayInner(receipt, sizeof(receipt)) ==
         tlp::V2RelayCodecStatus::kRelayNotAllowed);

  // Never allow nested relay, unknown v2 family or old HISTORY_SECURE.
  uint8_t nested[64]{};
  nested[0] = 2U; nested[1] = 2U;
  assert(tlp::validateV2RelayInner(nested, sizeof(nested)) ==
         tlp::V2RelayCodecStatus::kInnerType);
  nested[1] = 3U;
  assert(tlp::validateV2RelayInner(nested, sizeof(nested)) ==
         tlp::V2RelayCodecStatus::kInnerType);
  return 0;
}
