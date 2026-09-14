#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "tlp_test_packet.h"
#include "tlp_position_packet.h"
#include "tlp_relay_forward_packet.h"

using namespace orun_tlp::tlp;

namespace {

// Independent, literal wire fixtures, audited against the protocol offset tables.
// Never construct these expectations with a serializer or an encoding helper.
constexpr uint8_t kTestGolden[] = {
    0x01, 0x01,                                      // 0..1: version, TEST
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,  // 2..9: source
    0x10, 0x20, 0x30, 0x40,                          // 10..13: sequence
    0x50, 0x60, 0x70, 0x80};                         // 14..17: uptime ms
constexpr TestPacket kTestInput{
    0x0123456789ABCDEFULL, 0x10203040, 0x50607080};

constexpr uint8_t kPositionGolden[] = {
    0x01, 0x02,                                      // 0..1: version, POSITION
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,  // 2..9: source
    0x91, 0xA2, 0xB3, 0xC4,                          // 10..13: sequence
    0x65, 0xA1, 0xB2, 0xC3,                          // 14..17: UTC seconds
    0xEB, 0xCE, 0xD9, 0x88,                          // 18..21: -338765432 E7
    0xD6, 0x34, 0x02, 0x79,                          // 22..25: -701234567 E7
    0xFF, 0xFF, 0xCF, 0xC7,                          // 26..29: -12345 mm
    0x01, 0x23,                                      // 30..31: HDOP x100 = 291
    0x0D, 0x07};                                    // 32..33: 13 sats, flags
constexpr PositionPacket kPositionInput{
    0x0123456789ABCDEFULL, 0x91A2B3C4, 0x65A1B2C3,
    -338765432, -701234567, -12345, 291, 13, 7};

// The full inner packet is deliberately repeated as literals, not concatenated
// from serializer output. A separate assertion freezes exact inner preservation.
constexpr uint8_t kRelayGolden[] = {
    0x01, 0x03,                                      // 0..1: version, RELAY
    0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67,  // 2..9: relay ID
    0x01, 0x22,                                      // 10..11: hop, inner size
    0xFF, 0x93, 0xF7,                                // 12..14: RSSI -109, SNR -9
    0x01, 0x02,                                      // 15..16: inner v1 POSITION
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,  // 17..24: source
    0x91, 0xA2, 0xB3, 0xC4,                          // 25..28: sequence
    0x65, 0xA1, 0xB2, 0xC3,                          // 29..32: UTC
    0xEB, 0xCE, 0xD9, 0x88,                          // 33..36: latitude
    0xD6, 0x34, 0x02, 0x79,                          // 37..40: longitude
    0xFF, 0xFF, 0xCF, 0xC7,                          // 41..44: altitude
    0x01, 0x23, 0x0D, 0x07};                         // 45..48: HDOP/sats/flags

static_assert(kProtocolVersion == 1 && kPacketTypeTest == 0x01 &&
              kPacketTypePosition == 0x02 && kPacketTypeRelayForward == 0x03);
static_assert(sizeof(kTestGolden) == 18 && kTestPacketSize == 18);
static_assert(sizeof(kPositionGolden) == 34 && kPositionPacketSize == 34);
static_assert(sizeof(kRelayGolden) == 49 && kRelayForwardPacketSize == 49);
static_assert(kRelayForwardHeaderSize == 15 && kRelayHopCount == 1);
static_assert(kPositionFlagValidFix == 0x01 &&
              kPositionFlagValidUtcTime == 0x02 && kPositionFlag3dFix == 0x04);

void assertPositionFields(const PositionPacket& actual) {
  assert(actual.source_device_id == 0x0123456789ABCDEFULL);
  assert(actual.sequence_number == 0x91A2B3C4);
  assert(actual.gnss_utc_epoch_seconds == 0x65A1B2C3);
  assert(actual.latitude_e7 == -338765432);
  assert(actual.longitude_e7 == -701234567);
  assert(actual.altitude_mm == -12345);
  assert(actual.hdop_x100 == 291);
  assert(actual.satellites == 13 && actual.flags == 7);
}

RelayForwardPacket relayInput() {
  RelayForwardPacket input{};
  input.relay_device_id = 0x89ABCDEF01234567ULL;
  input.hop_count = 1;
  input.original_length = 34;
  input.ingress_rssi_dbm = -109;
  input.ingress_snr_db = -9;
  memcpy(input.original_packet, kPositionGolden, 34);
  return input;
}

void goldenVectors() {
  uint8_t test[18]{};
  assert(serializeTestPacket(kTestInput, test, sizeof(test)));
  assert(memcmp(test, kTestGolden, 18) == 0);
  TestPacket decoded_test{};
  assert(deserializeTestPacket(kTestGolden, 18, &decoded_test));
  assert(decoded_test.source_device_id == 0x0123456789ABCDEFULL);
  assert(decoded_test.sequence_number == 0x10203040);
  assert(decoded_test.uptime_ms == 0x50607080);

  uint8_t position[34]{};
  assert(serializePositionPacket(kPositionInput, position, sizeof(position)));
  assert(memcmp(position, kPositionGolden, 34) == 0);
  PositionPacket decoded_position{};
  assert(deserializePositionPacket(kPositionGolden, 34, &decoded_position));
  assertPositionFields(decoded_position);

  uint8_t relay[49]{};
  assert(serializeRelayForwardPacket(relayInput(), relay, sizeof(relay)));
  assert(memcmp(relay, kRelayGolden, 49) == 0);
  assert(memcmp(relay + 15, kPositionGolden, 34) == 0);
  assert(memcmp(kRelayGolden + 15, kPositionGolden, 34) == 0);
  RelayForwardPacket decoded_relay{};
  assert(deserializeRelayForwardPacket(kRelayGolden, 49, &decoded_relay,
                                      &decoded_position) == RelayDecodeStatus::kOk);
  assert(decoded_relay.relay_device_id == 0x89ABCDEF01234567ULL);
  assert(decoded_relay.hop_count == 1 && decoded_relay.original_length == 34);
  assert(decoded_relay.ingress_rssi_dbm == -109);
  assert(decoded_relay.ingress_snr_db == -9);
  assert(memcmp(decoded_relay.original_packet, kPositionGolden, 34) == 0);
  assertPositionFields(decoded_position);
}

void nullAndLengthRejection() {
  TestPacket test{};
  PositionPacket position{};
  RelayForwardPacket relay{};
  assert(!serializeTestPacket(kTestInput, nullptr, 18));
  assert(!deserializeTestPacket(nullptr, 18, &test));
  assert(!deserializeTestPacket(kTestGolden, 18, nullptr));
  assert(!serializePositionPacket(kPositionInput, nullptr, 34));
  assert(!deserializePositionPacket(nullptr, 34, &position));
  assert(!deserializePositionPacket(kPositionGolden, 34, nullptr));
  assert(!serializeRelayForwardPacket(relayInput(), nullptr, 49));
  assert(deserializeRelayForwardPacket(nullptr, 49, &relay, &position) ==
         RelayDecodeStatus::kLength);
  assert(deserializeRelayForwardPacket(kRelayGolden, 49, nullptr, &position) ==
         RelayDecodeStatus::kLength);
  assert(deserializeRelayForwardPacket(kRelayGolden, 49, &relay, nullptr) ==
         RelayDecodeStatus::kLength);

  // Padded backing arrays make the overlength cases genuine accessible input.
  // Every truncation (including 0 and relay's 15-byte boundary) is exercised.
  uint8_t input[50]{};
  uint8_t output[50]{};
  memcpy(input, kTestGolden, 18);
  for (size_t size = 0; size <= 19; ++size) {
    if (size == 18) continue;
    assert(!serializeTestPacket(kTestInput, output, size));
    assert(!deserializeTestPacket(input, size, &test));
  }
  memcpy(input, kPositionGolden, 34);
  for (size_t size = 0; size <= 35; ++size) {
    if (size == 34) continue;
    assert(!serializePositionPacket(kPositionInput, output, size));
    assert(!deserializePositionPacket(input, size, &position));
  }
  memcpy(input, kRelayGolden, 49);
  for (size_t size = 0; size <= 50; ++size) {
    if (size == 49) continue;
    assert(!serializeRelayForwardPacket(relayInput(), output, size));
    assert(deserializeRelayForwardPacket(input, size, &relay, &position) ==
           RelayDecodeStatus::kLength);
  }
}

void testAndPositionHeaders() {
  // All unsupported version/type byte values, from literal golden input.
  for (unsigned value = 0; value <= 255; ++value) {
    uint8_t bytes[34];
    TestPacket test{};
    PositionPacket position{};
    memcpy(bytes, kTestGolden, 18);
    bytes[0] = static_cast<uint8_t>(value);
    assert(deserializeTestPacket(bytes, 18, &test) == (value == 1));
    memcpy(bytes, kTestGolden, 18);
    bytes[1] = static_cast<uint8_t>(value);
    assert(deserializeTestPacket(bytes, 18, &test) == (value == 1));
    memcpy(bytes, kPositionGolden, 34);
    bytes[0] = static_cast<uint8_t>(value);
    assert(deserializePositionPacket(bytes, 34, &position) == (value == 1));
    memcpy(bytes, kPositionGolden, 34);
    bytes[1] = static_cast<uint8_t>(value);
    assert(deserializePositionPacket(bytes, 34, &position) == (value == 2));
  }
}

void positionBoundsAndFlags() {
  // Literal signed big-endian boundary bytes; no shared writeU32 algorithm.
  const struct { size_t offset; int32_t value; uint8_t bytes[4]; bool valid; } cases[] = {
      {18, -900000000, {0xCA, 0x5B, 0x17, 0x00}, true},
      {18,  900000000, {0x35, 0xA4, 0xE9, 0x00}, true},
      {18, -900000001, {0xCA, 0x5B, 0x16, 0xFF}, false},
      {18,  900000001, {0x35, 0xA4, 0xE9, 0x01}, false},
      {22, -1800000000, {0x94, 0xB6, 0x2E, 0x00}, true},
      {22,  1800000000, {0x6B, 0x49, 0xD2, 0x00}, true},
      {22, -1800000001, {0x94, 0xB6, 0x2D, 0xFF}, false},
      {22,  1800000001, {0x6B, 0x49, 0xD2, 0x01}, false}};
  for (const auto& value : cases) {
    PositionPacket input = kPositionInput, decoded{};
    if (value.offset == 18) input.latitude_e7 = value.value;
    else input.longitude_e7 = value.value;
    uint8_t expected[34], encoded[34]{};
    memcpy(expected, kPositionGolden, 34);
    memcpy(expected + value.offset, value.bytes, 4);
    assert(serializePositionPacket(input, encoded, 34) == value.valid);
    assert(deserializePositionPacket(expected, 34, &decoded) == value.valid);
    if (value.valid) {
      assert(memcmp(encoded, expected, 34) == 0);
      assert(decoded.latitude_e7 == input.latitude_e7);
      assert(decoded.longitude_e7 == input.longitude_e7);
    }
  }

  for (unsigned flags = 0; flags <= 255; ++flags) {
    PositionPacket input = kPositionInput, decoded{};
    input.flags = static_cast<uint8_t>(flags);
    uint8_t expected[34], encoded[34]{};
    memcpy(expected, kPositionGolden, 34);
    expected[33] = static_cast<uint8_t>(flags);
    assert(serializePositionPacket(input, encoded, 34) == (flags < 8));
    assert(deserializePositionPacket(expected, 34, &decoded) == (flags < 8));
    if (flags < 8) {
      assert(memcmp(encoded, expected, 34) == 0);
      assert(decoded.flags == flags);
      assert(decoded.gnss_utc_epoch_seconds == 0x65A1B2C3);
    }
  }
  // CURRENT codec semantics: 0,0 is legal; valid_fix need not be set; UTC/flag
  // consistency is not enforced here. Do not turn this freeze into new policy.
  uint8_t bytes[34], encoded[34]{};
  memcpy(bytes, kPositionGolden, 34);
  memset(bytes + 14, 0, 12); // UTC, latitude and longitude = 0, flags remain 7.
  PositionPacket input = kPositionInput, decoded{};
  input.gnss_utc_epoch_seconds = 0;
  input.latitude_e7 = input.longitude_e7 = 0;
  assert(serializePositionPacket(input, encoded, 34));
  assert(memcmp(encoded, bytes, 34) == 0);
  assert(deserializePositionPacket(bytes, 34, &decoded));
  assert(decoded.gnss_utc_epoch_seconds == 0 && decoded.latitude_e7 == 0 &&
         decoded.longitude_e7 == 0 && decoded.flags == 7);
}

void relayMalformed() {
  using Status = RelayDecodeStatus;
  const struct { size_t offset; uint8_t value; Status status; } cases[] = {
      {0, 0, Status::kVersion}, {0, 2, Status::kVersion},
      {1, 1, Status::kType}, {1, 0xFE, Status::kType},
      {10, 0, Status::kHopCount}, {10, 2, Status::kHopCount},
      {10, 255, Status::kHopCount},
      {11, 0, Status::kOriginalLength}, {11, 33, Status::kOriginalLength},
      {11, 35, Status::kOriginalLength}, {11, 255, Status::kOriginalLength},
      {15, 0, Status::kInnerVersion}, {15, 2, Status::kInnerVersion},
      {16, 3, Status::kNestedRelay}, {16, 1, Status::kInnerType},
      {16, 0xFE, Status::kInnerType},
      {48, 0x08, Status::kMalformedPosition},
      {48, 0x80, Status::kMalformedPosition},
      {33, 0x7F, Status::kMalformedPosition}, // latitude exceeds +90 E7
      {37, 0x7F, Status::kMalformedPosition}}; // longitude exceeds +180 E7
  for (const auto& value : cases) {
    uint8_t bytes[49], output[49]{};
    memcpy(bytes, kRelayGolden, 49);
    bytes[value.offset] = value.value;
    RelayForwardPacket decoded{}, input = relayInput();
    PositionPacket position{};
    assert(deserializeRelayForwardPacket(bytes, 49, &decoded, &position) ==
           value.status);
    // Serializer has no outer version/type input, but rejects bad metadata and
    // malformed inner bytes. Its fixed inner array is never read with bad size.
    if (value.offset == 10) input.hop_count = value.value;
    else if (value.offset == 11) input.original_length = value.value;
    else if (value.offset >= 15)
      memcpy(input.original_packet, bytes + 15, 34);
    else continue;
    assert(!serializeRelayForwardPacket(input, output, 49));
  }
}

}  // namespace

int main() {
  goldenVectors();
  nullAndLengthRejection();
  testAndPositionHeaders();
  positionBoundsAndFlags();
  relayMalformed();
  puts("B1A independent legacy packet golden/malformed checks: PASS");
}
