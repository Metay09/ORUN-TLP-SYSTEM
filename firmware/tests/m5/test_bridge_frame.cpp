#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bridge_frame.h"
#include "tlp_position_packet.h"

using namespace orun_tlp;

namespace {

// Golden lines were produced by an independent implementation of
// protocol/BRIDGE_FRAME_V1.md (not by bridge_frame.cpp). A host decoder must
// accept exactly these lines; change them only together with the contract.
constexpr char kDirectGolden[] =
    "BRIDGE v=1 kind=POSITION n=1 up=123456 node=0102030405060708 dup=0 "
    "path=DIRECT rssi=-82 snr=6 "
    "raw=010289ABCDEF01234567000000286553F10018701A8011490C800000303900AF0907 "
    "crc=8E07";

constexpr char kRelayExtremeGolden[] =
    "BRIDGE v=1 kind=POSITION n=4294967295 up=4294967295 "
    "node=0123456789ABCDEF dup=1 path=RELAY relay=FEDCBA9876543210 "
    "in_rssi=-32768 in_snr=-128 rssi=-32768 snr=-128 "
    "raw=0102FFFFFFFFFFFFFFFFFFFFFFFF00000000CA5B170094B62E0080000000FFFFFF00 "
    "crc=792F";

uint8_t hexNibble(char value) {
  if (value >= '0' && value <= '9') return static_cast<uint8_t>(value - '0');
  assert(value >= 'A' && value <= 'F');
  return static_cast<uint8_t>(value - 'A' + 10);
}

void rawFromGolden(const char* golden, uint8_t* packet) {
  const char* raw = strstr(golden, " raw=");
  assert(raw != nullptr);
  raw += 5;
  for (size_t index = 0; index < tlp::kPositionPacketSize; ++index) {
    packet[index] = static_cast<uint8_t>((hexNibble(raw[2 * index]) << 4) |
                                         hexNibble(raw[2 * index + 1]));
  }
}

void checksumMatchesPublishedCheckValue() {
  // CRC-16/CCITT-FALSE catalogue check value.
  assert(bridge_frame::crc16("123456789", 9) == 0x29B1);
  assert(bridge_frame::crc16("", 0) == 0xFFFF);
}

void directLineIsExactAndCarriesTheFrozenV1Packet() {
  bridge_frame::ReceivedPosition received{};
  received.line_number = 1;
  received.uptime_ms = 123456;
  received.receiver_device_id = 0x0102030405060708ULL;
  received.link_rssi_dbm = -82;
  received.link_snr_db = 6;
  // Relay-only fields must not leak into a direct line.
  received.relay_device_id = 0xFEDCBA9876543210ULL;
  received.ingress_rssi_dbm = -110;
  received.ingress_snr_db = -9;
  const tlp::PositionPacket position{0x89ABCDEF01234567ULL, 40, 1700000000,
                                     410000000, 290000000, 12345, 175, 9, 7};
  assert(tlp::serializePositionPacket(position, received.packet,
                                      sizeof(received.packet)));

  // The golden raw field is the frozen TLP v1 encoding of that position.
  uint8_t golden_packet[tlp::kPositionPacketSize];
  rawFromGolden(kDirectGolden, golden_packet);
  assert(memcmp(golden_packet, received.packet, sizeof(golden_packet)) == 0);

  char line[bridge_frame::kMaxLineSize];
  memset(line, 'x', sizeof(line));
  const size_t length =
      bridge_frame::formatPositionLine(received, line, sizeof(line));
  assert(length == strlen(kDirectGolden));
  assert(strcmp(line, kDirectGolden) == 0);
  assert(strchr(line, '\n') == nullptr && strchr(line, '\r') == nullptr);

  // A host can decode the packet straight from the line.
  uint8_t decoded_bytes[tlp::kPositionPacketSize];
  rawFromGolden(line, decoded_bytes);
  tlp::PositionPacket decoded{};
  assert(tlp::deserializePositionPacket(decoded_bytes, sizeof(decoded_bytes),
                                        &decoded));
  assert(decoded.source_device_id == position.source_device_id &&
         decoded.sequence_number == 40 &&
         decoded.latitude_e7 == 410000000 &&
         decoded.longitude_e7 == 290000000 &&
         decoded.gnss_utc_epoch_seconds == 1700000000);

  received.duplicate = true;
  assert(bridge_frame::formatPositionLine(received, line, sizeof(line)) != 0);
  assert(strstr(line, " dup=1 path=DIRECT rssi=-82 snr=6 raw=") != nullptr);
  assert(strstr(line, "relay=") == nullptr && strstr(line, "in_rssi") == nullptr);
}

void relayLineWithExtremeValuesIsExactAndBounded() {
  bridge_frame::ReceivedPosition received{};
  received.line_number = UINT32_MAX;
  received.uptime_ms = UINT32_MAX;
  received.receiver_device_id = 0x0123456789ABCDEFULL;
  received.duplicate = true;
  received.relayed = true;
  received.relay_device_id = 0xFEDCBA9876543210ULL;
  received.ingress_rssi_dbm = INT16_MIN;
  received.ingress_snr_db = INT8_MIN;
  received.link_rssi_dbm = INT16_MIN;
  received.link_snr_db = INT8_MIN;
  // The formatter is a transparent carrier: it prints the bytes it is given
  // and does not interpret them.
  rawFromGolden(kRelayExtremeGolden, received.packet);

  char line[bridge_frame::kMaxLineSize];
  const size_t length =
      bridge_frame::formatPositionLine(received, line, sizeof(line));
  assert(strcmp(line, kRelayExtremeGolden) == 0);
  // Longest possible line: every numeric field at its widest.
  assert(length == 243 && length < bridge_frame::kMaxLineSize);

  // Positive link values print without a sign.
  received.ingress_rssi_dbm = 0;
  received.ingress_snr_db = INT8_MAX;
  received.link_rssi_dbm = INT16_MAX;
  received.link_snr_db = 0;
  assert(bridge_frame::formatPositionLine(received, line, sizeof(line)) != 0);
  assert(strstr(line, " in_rssi=0 in_snr=127 rssi=32767 snr=0 raw=") != nullptr);
}

void checksumCoversEverythingBeforeItsOwnField() {
  bridge_frame::ReceivedPosition received{};
  received.line_number = 7;
  char line[bridge_frame::kMaxLineSize];
  assert(bridge_frame::formatPositionLine(received, line, sizeof(line)) != 0);
  const char* checksum = strstr(line, " crc=");
  assert(checksum != nullptr && strlen(checksum) == 9);
  char expected[8];
  snprintf(expected, sizeof(expected), "%04X",
           static_cast<unsigned>(bridge_frame::crc16(
               line, static_cast<size_t>(checksum - line))));
  assert(strcmp(checksum + 5, expected) == 0);

  // Any other observation changes the checksum-protected text.
  char other[bridge_frame::kMaxLineSize];
  received.line_number = 8;
  assert(bridge_frame::formatPositionLine(received, other, sizeof(other)) != 0);
  assert(strcmp(line, other) != 0);
}

void shortOutputNeverProducesATruncatedLine() {
  bridge_frame::ReceivedPosition received{};
  received.line_number = 1;
  char reference[bridge_frame::kMaxLineSize];
  const size_t length =
      bridge_frame::formatPositionLine(received, reference, sizeof(reference));
  assert(length != 0);

  char exact[bridge_frame::kMaxLineSize];
  assert(bridge_frame::formatPositionLine(received, exact, length + 1) ==
         length);
  assert(strcmp(exact, reference) == 0);

  for (size_t size = 1; size <= length; ++size) {
    char small[bridge_frame::kMaxLineSize];
    memset(small, 'x', sizeof(small));
    assert(bridge_frame::formatPositionLine(received, small, size) == 0);
    assert(small[0] == '\0');
    // Nothing is written past the offered size.
    assert(small[size] == 'x');
  }
  assert(bridge_frame::formatPositionLine(received, nullptr, 64) == 0);
  char untouched = 'x';
  assert(bridge_frame::formatPositionLine(received, &untouched, 0) == 0);
  assert(untouched == 'x');
}

}  // namespace

int main() {
  checksumMatchesPublishedCheckValue();
  directLineIsExactAndCarriesTheFrozenV1Packet();
  relayLineWithExtremeValuesIsExactAndBounded();
  checksumCoversEverythingBeforeItsOwnField();
  shortOutputNeverProducesATruncatedLine();
  puts("Host bridge line v1 golden/bounds checks: PASS");
}
