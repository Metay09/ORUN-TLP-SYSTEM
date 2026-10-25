#include "step3b_gateway_enrollment.h"

#if defined(ORUN_STEP3B_GATEWAY_ENROLLMENT)

#include <string.h>

#include "observation_store_format.h"

namespace orun_tlp {
namespace {
namespace fmt = gateway_grant_format;

constexpr char kPrefix[] = "BENCH OGK1 ";
constexpr size_t kPrefixLength = sizeof(kPrefix) - 1U;
constexpr uint32_t kChunk = 64U;  // flash program size (<= gate staging)
constexpr size_t kMaxDataBytes = 64U;

void secureWipe(void* dest, size_t n) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(dest);
  while (n-- != 0U) *p++ = 0U;
}
uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24U) | (uint32_t(p[1]) << 16U) |
         (uint32_t(p[2]) << 8U) | uint32_t(p[3]);
}
uint64_t be64(const uint8_t* p) {
  return (uint64_t(be32(p)) << 32U) | be32(p + 4U);
}
int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;  // lowercase is rejected: the tool sends uppercase only
}
bool hexValue(const char* text, size_t length, uint32_t& value) {
  if (length == 0U || length > 8U) return false;
  value = 0U;
  for (size_t i = 0U; i < length; ++i) {
    const int n = nibble(text[i]);
    if (n < 0) return false;
    value = (value << 4U) | uint32_t(n);
  }
  return true;
}
bool hexBytes(const char* text, size_t length, uint8_t* out, size_t& count) {
  if (length == 0U || (length & 1U) != 0U || length / 2U > kMaxDataBytes)
    return false;
  for (size_t i = 0U; i < length; i += 2U) {
    const int hi = nibble(text[i]);
    const int lo = nibble(text[i + 1U]);
    if (hi < 0 || lo < 0) return false;
    out[i / 2U] = static_cast<uint8_t>((hi << 4) | lo);
  }
  count = length / 2U;
  return true;
}
bool is(const char* text, size_t length, const char* word) {
  const size_t n = strlen(word);
  return length == n && memcmp(text, word, n) == 0;
}
}  // namespace

bool Step3bStagedSnapshotFlash::read(uint32_t offset, void* data,
                                     size_t size) const {
  constexpr uint32_t kRegion = fmt::kPageBytes * fmt::kPageCount;
  if (image_ == nullptr || data == nullptr || offset > kRegion ||
      size > kRegion - offset)
    return false;
  uint8_t* out = static_cast<uint8_t*>(data);
  for (size_t i = 0U; i < size; ++i) {
    const uint32_t at = offset + static_cast<uint32_t>(i);
    out[i] = at < fmt::kStoredBytes ? image_[at] : 0xFFU;
  }
  return true;
}

Step3bGrantInstaller::~Step3bGrantInstaller() { wipe(); }

void Step3bGrantInstaller::wipe() {
  secureWipe(image_, sizeof(image_));
  received_ = 0U;
}

void Step3bGrantInstaller::fault() {
  wipe();
  waiting_ = false;
  state_ = State::kFault;
}

bool Step3bGrantInstaller::exactRead(uint32_t offset, const uint8_t* expected,
                                     size_t length) const {
  uint8_t block[64]{};
  for (size_t n = 0U; n < length; n += sizeof(block)) {
    const size_t count = length - n < sizeof(block) ? length - n : sizeof(block);
    if (!flash_.read(offset + static_cast<uint32_t>(n), block, count) ||
        memcmp(block, expected + n, count) != 0)
      return false;
  }
  return true;
}

bool Step3bGrantInstaller::erasedFrom(uint16_t page, uint32_t from) const {
  const uint32_t base = uint32_t(page) * fmt::kPageBytes;
  uint8_t block[64]{};
  for (uint32_t off = from; off < fmt::kPageBytes; off += sizeof(block)) {
    const size_t n = fmt::kPageBytes - off < sizeof(block)
                         ? fmt::kPageBytes - off : sizeof(block);
    if (!flash_.read(base + off, block, n)) return false;
    for (size_t i = 0U; i < n; ++i)
      if (block[i] != 0xFFU) return false;
  }
  return true;
}

Step3bGrantInstaller::Reply Step3bGrantInstaller::handleLine(
    const char* line, size_t length, GatewayGrantSnapshotReader& current) {
  if (line == nullptr || length < kPrefixLength ||
      memcmp(line, kPrefix, kPrefixLength) != 0)
    return Reply::kNotMine;
  while (length > kPrefixLength &&
         (line[length - 1U] == ' ' || line[length - 1U] == '\r'))
    --length;
  const char* p = line + kPrefixLength;
  const size_t n = length - kPrefixLength;
  const char* space = static_cast<const char*>(memchr(p, ' ', n));
  const size_t word = space != nullptr ? size_t(space - p) : n;
  const char* rest = space != nullptr ? space + 1 : p + n;
  const size_t rest_length = n - (space != nullptr ? word + 1U : word);

  if (state_ == State::kWriting || state_ == State::kDone ||
      state_ == State::kFault)
    return Reply::kBusy;

  if (is(p, n, "BEGIN")) {
    wipe();
    candidate_generation_ = 0U;
    state_ = State::kReceiving;
    return Reply::kOk;
  }
  if (is(p, n, "ABORT")) {
    wipe();
    state_ = State::kIdle;
    return Reply::kAborted;
  }
  if (is(p, word, "DATA")) {
    if (state_ != State::kReceiving) return Reply::kNotReceiving;
    const char* gap = static_cast<const char*>(memchr(rest, ' ', rest_length));
    if (gap == nullptr) return Reply::kMalformed;
    uint32_t offset = 0U;
    uint8_t bytes[kMaxDataBytes]{};
    size_t count = 0U;
    const size_t hex_length = rest_length - size_t(gap - rest) - 1U;
    if (size_t(gap - rest) != 4U || !hexValue(rest, 4U, offset) ||
        !hexBytes(gap + 1, hex_length, bytes, count)) {
      secureWipe(bytes, sizeof(bytes));
      return Reply::kMalformed;
    }
    if (offset != received_ || count > fmt::kStoredBytes - received_) {
      secureWipe(bytes, sizeof(bytes));
      return Reply::kBadOffset;
    }
    memcpy(image_ + received_, bytes, count);
    secureWipe(bytes, sizeof(bytes));
    received_ += static_cast<uint32_t>(count);
    return Reply::kOk;
  }
  if (is(p, word, "COMMIT")) {
    if (state_ != State::kReceiving) return Reply::kNotReceiving;
    uint32_t crc = 0U;
    if (rest_length != 8U || !hexValue(rest, 8U, crc)) return Reply::kMalformed;
    const Reply r = commit(crc, current);
    if (r != Reply::kWriteStarted) {
      // Nothing was written: drop the candidate; a new BEGIN may follow.
      wipe();
      state_ = State::kIdle;
    }
    return r;
  }
  return Reply::kMalformed;
}

Step3bGrantInstaller::Reply Step3bGrantInstaller::commit(
    uint32_t crc, GatewayGrantSnapshotReader& current) {
  if (received_ != fmt::kStoredBytes) return Reply::kIncomplete;
  if (observation_store_format::crc32(image_, fmt::kStoredBytes) != crc)
    return Reply::kBadCrc;
  if (be64(image_ + 8U) != gateway_id_) return Reply::kWrongGateway;

  // The installed snapshot is the rollback anchor: it must be readable
  // (verified) or the region blank. Unknown, foreign (e.g. ORS1) or torn
  // pages are never overwritten implicitly.
  const auto status = current.status();
  if ((status != GatewayGrantSnapshotReader::Status::kReady &&
       status != GatewayGrantSnapshotReader::Status::kEmpty) ||
      (status == GatewayGrantSnapshotReader::Status::kReady &&
       !current.verifiedForLocalGateway(gateway_id_)) ||
      current.localGatewayId() != gateway_id_ ||
      flash_.hasUnreconciledMutation())
    return Reply::kRegionNotReady;

  // Verify the candidate with the SAME reader logic and pinned issuer as
  // the boot path, over a read-only view of the staged bytes.
  Step3bStagedSnapshotFlash staged(image_);
  uint64_t generation = 0U;
  {
    // Heap, not the 8 KiB loop stack: the reader keeps a full image copy and
    // the CC310 verifier needs its own ~3 KiB of stack.
    GatewayGrantSnapshotReader* candidate =
        new GatewayGrantSnapshotReader(staged, issuer_);
    if (candidate == nullptr) return Reply::kRegionNotReady;
    if (candidate->begin(gateway_id_, true, false) ==
        GatewayGrantSnapshotReader::Status::kReady)
      generation = candidate->activeGeneration();
    delete candidate;  // ~reader wipes its copy of the grant keys
  }
  if (generation == 0U || generation != be64(image_ + 16U))
    return Reply::kNotAuthentic;

  const uint64_t installed = status == GatewayGrantSnapshotReader::Status::kReady
                                 ? current.activeGeneration() : 0U;
  if (generation <= installed) return Reply::kNotNewer;

  uint16_t destination = 0U;
  if (installed != 0U) {
    uint16_t source = UINT16_MAX;
    for (uint16_t page = 0U; page < fmt::kPageCount; ++page) {
      uint8_t header[24]{};
      if (!flash_.read(uint32_t(page) * fmt::kPageBytes, header, sizeof(header)))
        return Reply::kRegionNotReady;
      if (be32(header) == fmt::kMagic && be64(header + 16U) == installed)
        source = page;
    }
    if (source == UINT16_MAX) return Reply::kRegionNotReady;
    destination = static_cast<uint16_t>(1U - source);
  }

  // No ACK may use the old authorization while its replacement is being
  // written. Latched for this boot even if a later flash step fails.
  current.denyForCurrentBoot();
  candidate_generation_ = generation;
  destination_ = destination;
  phase_ = Phase::kErase;
  waiting_ = false;
  offset_ = 0U;
  length_ = 0U;
  state_ = State::kWriting;
  return Reply::kWriteStarted;
}

Step3bGrantInstaller::State Step3bGrantInstaller::poll() {
  if (state_ != State::kWriting) return state_;
  if (flash_.hasUnreconciledMutation()) {
    fault();
    return state_;
  }
  const uint32_t base = uint32_t(destination_) * fmt::kPageBytes;

  if (!waiting_ && phase_ == Phase::kBody && offset_ >= fmt::kCommitOffset) {
    // Body complete: every byte read back and the commit word still erased
    // before the irreversible commit marker.
    if (!exactRead(base, image_, fmt::kCommitOffset) ||
        !erasedFrom(destination_, fmt::kCommitOffset)) {
      fault();
      return state_;
    }
    phase_ = Phase::kCommit;
    return state_;
  }
  if (!waiting_ && phase_ == Phase::kBody) {
    const uint32_t remaining = fmt::kCommitOffset - offset_;
    length_ = remaining < kChunk ? remaining : kChunk;
    if ((length_ & 3U) != 0U) {
      fault();
      return state_;
    }
  }

  const uint8_t commit_word[4]{};
  const FlashOpResult op =
      waiting_ ? flash_.pollPending()
      : phase_ == Phase::kErase ? flash_.erasePage(destination_)
      : phase_ == Phase::kBody
          ? flash_.program(base + offset_, image_ + offset_, length_)
          : flash_.program(base + fmt::kCommitOffset, commit_word,
                           sizeof(commit_word));
  if (op == FlashOpResult::kPending) {
    waiting_ = true;
    return state_;
  }
  waiting_ = false;
  if (op != FlashOpResult::kDone) {
    fault();
    return state_;
  }

  switch (phase_) {
    case Phase::kErase:
      if (!erasedFrom(destination_, 0U)) fault();
      else phase_ = Phase::kBody;
      return state_;
    case Phase::kBody:
      if (!exactRead(base + offset_, image_ + offset_, length_)) {
        fault();
        return state_;
      }
      offset_ += length_;
      return state_;
    case Phase::kCommit:
      if (!exactRead(base, image_, fmt::kStoredBytes) ||
          !erasedFrom(destination_, fmt::kStoredBytes)) {
        fault();
        return state_;
      }
      wipe();
      state_ = State::kDone;  // used after the next boot
      return state_;
  }
  return state_;
}

const char* Step3bGrantInstaller::replyName(Reply reply) {
  switch (reply) {
    case Reply::kNotMine: return "not_mine";
    case Reply::kOk: return "ok";
    case Reply::kAborted: return "aborted";
    case Reply::kWriteStarted: return "write_started";
    case Reply::kMalformed: return "malformed";
    case Reply::kBusy: return "busy_reboot_first";
    case Reply::kNotReceiving: return "no_begin";
    case Reply::kBadOffset: return "bad_offset";
    case Reply::kIncomplete: return "incomplete";
    case Reply::kBadCrc: return "bad_crc";
    case Reply::kWrongGateway: return "wrong_gateway";
    case Reply::kNotAuthentic: return "not_authentic";
    case Reply::kNotNewer: return "not_newer";
    case Reply::kRegionNotReady: return "region_not_ready";
  }
  return "?";
}

const char* Step3bGrantInstaller::stateName(State state) {
  switch (state) {
    case State::kIdle: return "IDLE";
    case State::kReceiving: return "RECEIVING";
    case State::kWriting: return "WRITING";
    case State::kDone: return "DONE";
    case State::kFault: return "FAULT";
  }
  return "?";
}

}  // namespace orun_tlp
#endif
