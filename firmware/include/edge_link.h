#pragma once

#include <stddef.h>
#include <stdint.h>

namespace orun_tlp {

// SF5L6: Gateway <-> Edge (phone / PC) custody hand-off link, transport
// neutral. USB carries the same facts as text lines (EDGE HELD / EDGE ACCEPT,
// sf5l0_bench.h); BLE carries them as these binary messages split into
// 20-byte frames (ATT MTU 23, no MTU negotiation assumed, like M7P7F).
// Pure code: no Bluefruit/Arduino dependency, host-tested.
namespace edge_link {

// Frozen 128-bit UUIDs, canonical RFC 4122 printed order (reverse for
// Bluefruit's BLEUuid, as M7P7G does). Separate from the M7P7F application
// service, whose 48-byte logical limit cannot carry a 100-byte custody object.
// ORUN Edge service:            6f9a1c20-5b3e-4d8a-9e61-2c4b7a0d3e51
// Edge -> Gateway (WRITE):      6f9a1c21-5b3e-4d8a-9e61-2c4b7a0d3e51
// Gateway -> Edge (INDICATE):   6f9a1c22-5b3e-4d8a-9e61-2c4b7a0d3e51
constexpr uint8_t kServiceUuid128[16] = {
    0x6F, 0x9A, 0x1C, 0x20, 0x5B, 0x3E, 0x4D, 0x8A,
    0x9E, 0x61, 0x2C, 0x4B, 0x7A, 0x0D, 0x3E, 0x51};
constexpr uint8_t kToGatewayUuid128[16] = {
    0x6F, 0x9A, 0x1C, 0x21, 0x5B, 0x3E, 0x4D, 0x8A,
    0x9E, 0x61, 0x2C, 0x4B, 0x7A, 0x0D, 0x3E, 0x51};
constexpr uint8_t kToEdgeUuid128[16] = {
    0x6F, 0x9A, 0x1C, 0x22, 0x5B, 0x3E, 0x4D, 0x8A,
    0x9E, 0x61, 0x2C, 0x4B, 0x7A, 0x0D, 0x3E, 0x51};

// ---- Framing -------------------------------------------------------------
// frame = version(1) | flags(1) | message id(1) | fragment index(1) | data
constexpr uint8_t kVersion = 0x01;
constexpr uint8_t kFrameSize = 20;
constexpr uint8_t kFrameHeader = 4;
constexpr uint8_t kFrameData = kFrameSize - kFrameHeader;  // 16
constexpr uint8_t kFlagStart = 0x01;
constexpr uint8_t kFlagEnd = 0x02;
constexpr size_t kMaxMessage = 160;
constexpr uint8_t kMaxFragments = (kMaxMessage + kFrameData - 1) / kFrameData;
constexpr uint32_t kReassemblyTimeoutMs = 3000;

// Splits ONE logical message into frames; stop-and-wait friendly.
class Fragmenter {
 public:
  // false: empty or too large message (nothing changes).
  bool load(const uint8_t* message, size_t size, uint8_t message_id);
  bool pending() const { return size_ != 0U; }
  // Current frame (unchanged until advance()). false if nothing pending.
  bool peek(uint8_t (&frame)[kFrameSize], uint8_t& length) const;
  void advance();  // the current frame was delivered (HVC confirmed)
  void clear() { size_ = 0; offset_ = 0; index_ = 0; }

 private:
  uint8_t message_[kMaxMessage]{};
  size_t size_ = 0;
  size_t offset_ = 0;
  uint8_t index_ = 0;
  uint8_t id_ = 0;
};

// Rebuilds one logical message from frames. Any gap, reorder, version or
// size violation drops the partial message (the sender's higher layer
// retries); it never returns a partial message.
class Reassembler {
 public:
  enum class Result : uint8_t { kNeedMore, kComplete, kDropped };
  Result push(const uint8_t* frame, size_t length, uint32_t now_ms);
  void poll(uint32_t now_ms);  // expires a stale partial message
  const uint8_t* message() const { return message_; }
  size_t size() const { return complete_ ? size_ : 0U; }
  void reset() { active_ = false; complete_ = false; size_ = 0; }

 private:
  uint8_t message_[kMaxMessage]{};
  size_t size_ = 0;
  uint8_t id_ = 0;
  uint8_t next_index_ = 0;
  uint32_t started_ms_ = 0;
  bool active_ = false;
  bool complete_ = false;
};

// ---- Messages ------------------------------------------------------------
enum class Type : uint8_t {
  // Gateway -> Edge
  kHello = 0x01,        // gateway id(8) held(2) capacity(2)
  kHeld = 0x02,         // page(2) slot(2) gen(8) flags(1) rssi(2) snr(1) len(1) obj
  kDumpEnd = 0x03,      // count(2): every record held right now was sent once
  kAcceptResult = 0x04, // page(2) slot(2) code(1)
  kReclaimed = 0x05,    // page(2) ok(1)
  // Compatible optional preview of the newest custody append. Exactly the
  // HELD payload, but NOT part of the oldest-first DUMP_END census. Older
  // phones safely ignore it and still receive every ordinary HELD in order.
  kHeldPreview = 0x06,
  // Edge -> Gateway
  kDump = 0x81,         // (empty) send every held record again from the start
  kAccept = 0x82,       // page(2) slot(2) gen(8) mac(32)
};
constexpr uint8_t kHeldFlagLink = 0x01;  // rssi/snr are this record's live RX

enum class AcceptCode : uint8_t {
  kHandedOff = 0, kAlready = 1, kUnknown = 2, kBusy = 3, kBadMac = 4,
  kMalformed = 5, kRejected = 6, kFailed = 7,
};

constexpr size_t kHelloSize = 1 + 8 + 2 + 2;
constexpr size_t kHeldHeader = 1 + 2 + 2 + 8 + 1 + 2 + 1 + 1;  // 18
constexpr size_t kAcceptSize = 1 + 2 + 2 + 8 + 32;              // 45

struct Held {
  uint16_t page = 0;
  uint16_t slot = 0;
  uint64_t generation = 0;
  bool has_link = false;
  int16_t rssi = 0;
  int8_t snr = 0;
  uint8_t size = 0;
  const uint8_t* object = nullptr;
};
struct Accept {
  uint16_t page = 0;
  uint16_t slot = 0;
  uint64_t generation = 0;
  uint8_t mac[32]{};
};

size_t encodeHello(uint64_t gateway_id, uint16_t held, uint16_t capacity,
                   uint8_t* out, size_t capacity_bytes);
size_t encodeHeld(const Held& held, uint8_t* out, size_t capacity);
size_t encodeDumpEnd(uint16_t count, uint8_t* out, size_t capacity);
size_t encodeAcceptResult(uint16_t page, uint16_t slot, AcceptCode code,
                          uint8_t* out, size_t capacity);
size_t encodeReclaimed(uint16_t page, bool ok, uint8_t* out, size_t capacity);
size_t encodeDump(uint8_t* out, size_t capacity);
size_t encodeAccept(const Accept& accept, uint8_t* out, size_t capacity);
bool decodeAccept(const uint8_t* message, size_t size, Accept& out);

}  // namespace edge_link
}  // namespace orun_tlp
