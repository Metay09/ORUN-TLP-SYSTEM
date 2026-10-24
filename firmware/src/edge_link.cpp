#include "edge_link.h"

#include <string.h>

#include "monotonic_time.h"

namespace orun_tlp {
namespace edge_link {
namespace {
void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void put64(uint8_t* p, uint64_t v) {
  for (int i = 7; i >= 0; --i) { p[i] = uint8_t(v); v >>= 8; }
}
uint16_t get16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint64_t get64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}
}  // namespace

// ---- Fragmenter ----------------------------------------------------------
bool Fragmenter::load(const uint8_t* message, size_t size, uint8_t id) {
  if (message == nullptr || size == 0U || size > kMaxMessage) return false;
  memcpy(message_, message, size);
  size_ = size;
  offset_ = 0;
  index_ = 0;
  id_ = id;
  return true;
}

bool Fragmenter::peek(uint8_t (&frame)[kFrameSize], uint8_t& length) const {
  if (size_ == 0U) return false;
  const size_t chunk =
      size_ - offset_ < kFrameData ? size_ - offset_ : kFrameData;
  frame[0] = kVersion;
  frame[1] = uint8_t((index_ == 0U ? kFlagStart : 0U) |
                     (offset_ + chunk == size_ ? kFlagEnd : 0U));
  frame[2] = id_;
  frame[3] = index_;
  memcpy(frame + kFrameHeader, message_ + offset_, chunk);
  length = uint8_t(kFrameHeader + chunk);
  return true;
}

void Fragmenter::advance() {
  if (size_ == 0U) return;
  const size_t chunk =
      size_ - offset_ < kFrameData ? size_ - offset_ : kFrameData;
  offset_ += chunk;
  ++index_;
  if (offset_ >= size_) clear();
}

// ---- Reassembler ---------------------------------------------------------
Reassembler::Result Reassembler::push(const uint8_t* frame, size_t length,
                                      uint32_t now_ms) {
  complete_ = false;
  if (frame == nullptr || length <= kFrameHeader || length > kFrameSize ||
      frame[0] != kVersion || (frame[1] & ~(kFlagStart | kFlagEnd)) != 0U) {
    reset();
    return Result::kDropped;
  }
  const bool start = (frame[1] & kFlagStart) != 0U;
  const bool end = (frame[1] & kFlagEnd) != 0U;
  if (start) {
    if (frame[3] != 0U) { reset(); return Result::kDropped; }
    active_ = true;
    id_ = frame[2];
    next_index_ = 0;
    size_ = 0;
    started_ms_ = now_ms;
  } else if (!active_ || frame[2] != id_ || frame[3] != next_index_) {
    reset();
    return Result::kDropped;
  }
  const size_t chunk = length - kFrameHeader;
  if (size_ + chunk > kMaxMessage) { reset(); return Result::kDropped; }
  memcpy(message_ + size_, frame + kFrameHeader, chunk);
  size_ += chunk;
  ++next_index_;
  if (!end) return Result::kNeedMore;
  active_ = false;
  complete_ = true;
  return Result::kComplete;
}

void Reassembler::poll(uint32_t now_ms) {
  if (active_ && monotonic::elapsed(now_ms, started_ms_, kReassemblyTimeoutMs))
    reset();
}

// ---- Messages ------------------------------------------------------------
size_t encodeHello(uint64_t gateway_id, uint16_t held, uint16_t cap,
                   uint8_t* out, size_t capacity) {
  if (out == nullptr || capacity < kHelloSize) return 0;
  out[0] = uint8_t(Type::kHello);
  put64(out + 1, gateway_id);
  put16(out + 9, held);
  put16(out + 11, cap);
  return kHelloSize;
}

size_t encodeHeld(const Held& h, uint8_t* out, size_t capacity) {
  const size_t total = kHeldHeader + h.size;
  if (out == nullptr || h.object == nullptr || h.size == 0U ||
      total > capacity || total > kMaxMessage)
    return 0;
  out[0] = uint8_t(Type::kHeld);
  put16(out + 1, h.page);
  put16(out + 3, h.slot);
  put64(out + 5, h.generation);
  out[13] = h.has_link ? kHeldFlagLink : 0U;
  put16(out + 14, uint16_t(h.has_link ? h.rssi : 0));
  out[16] = uint8_t(h.has_link ? h.snr : 0);
  out[17] = h.size;
  memcpy(out + kHeldHeader, h.object, h.size);
  return total;
}

size_t encodeDumpEnd(uint16_t count, uint8_t* out, size_t capacity) {
  if (out == nullptr || capacity < 3U) return 0;
  out[0] = uint8_t(Type::kDumpEnd);
  put16(out + 1, count);
  return 3;
}

size_t encodeAcceptResult(uint16_t page, uint16_t slot, AcceptCode code,
                          uint8_t* out, size_t capacity) {
  if (out == nullptr || capacity < 6U) return 0;
  out[0] = uint8_t(Type::kAcceptResult);
  put16(out + 1, page);
  put16(out + 3, slot);
  out[5] = uint8_t(code);
  return 6;
}

size_t encodeReclaimed(uint16_t page, bool ok, uint8_t* out, size_t capacity) {
  if (out == nullptr || capacity < 4U) return 0;
  out[0] = uint8_t(Type::kReclaimed);
  put16(out + 1, page);
  out[3] = ok ? 1U : 0U;
  return 4;
}

size_t encodeDump(uint8_t* out, size_t capacity) {
  if (out == nullptr || capacity < 1U) return 0;
  out[0] = uint8_t(Type::kDump);
  return 1;
}

size_t encodeAccept(const Accept& a, uint8_t* out, size_t capacity) {
  if (out == nullptr || capacity < kAcceptSize) return 0;
  out[0] = uint8_t(Type::kAccept);
  put16(out + 1, a.page);
  put16(out + 3, a.slot);
  put64(out + 5, a.generation);
  memcpy(out + 13, a.mac, sizeof(a.mac));
  return kAcceptSize;
}

bool decodeAccept(const uint8_t* m, size_t size, Accept& out) {
  if (m == nullptr || size != kAcceptSize || m[0] != uint8_t(Type::kAccept))
    return false;
  out.page = get16(m + 1);
  out.slot = get16(m + 3);
  out.generation = get64(m + 5);
  memcpy(out.mac, m + 13, sizeof(out.mac));
  return true;
}

}  // namespace edge_link
}  // namespace orun_tlp
