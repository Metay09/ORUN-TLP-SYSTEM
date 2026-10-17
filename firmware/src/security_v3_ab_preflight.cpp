#include "security_v3_ab_preflight.h"

#include <string.h>
#include "journal_format.h"
#include "security_format.h"
#include "security_v3_format.h"
#include "storage_config.h"

namespace orun_tlp {
namespace security_v3_ab_preflight {
namespace {
namespace jf = journal_format;
namespace sf = security_format;
namespace v3 = security_v3_format;

constexpr uint32_t kPageSize = storage_config::kPageSize;
constexpr uint32_t kPages = storage_config::kFutureSecurityRegionPages;
static_assert(kPages == 2U && kPageSize == 4096U,
              "SecurityStore A/B layout must not change");
static_assert(v3::kJournalOffset ==
              sf::kPageHeaderSize + sf::kCredentialRecordSize,
              "v3 journal offset must preserve v2 credential geometry");

bool validId(const uint8_t* id) {
  bool zero = true;
  bool erased = true;
  for (unsigned i = 0U; i < sf::kCredentialIdSize; ++i) {
    zero = zero && id[i] == 0U;
    erased = erased && id[i] == 0xFFU;
  }
  return !zero && !erased;
}

struct Page {
  bool present = false;
  uint8_t version = 0U;
  uint64_t generation = 0U;
  sf::Credential credential{};
  v3::Snapshot snapshot{};
};

enum class PageResult : uint8_t {
  kAbsent, kStaged, kPresent, kForeign, kUnsupported, kFault, kReadFault
};

bool read(const FlashBackend& flash, unsigned page, uint32_t offset,
          void* data, size_t len) {
  if (page >= kPages || offset > kPageSize ||
      len > static_cast<size_t>(kPageSize - offset))
    return false;
  return flash.read(static_cast<uint32_t>(page) * kPageSize + offset,
                    data, len);
}

class JournalFlashView : public v3::JournalReader {
 public:
  JournalFlashView(const FlashBackend& flash, unsigned page)
      : flash_(flash), page_(page) {}
  bool read(size_t offset, uint8_t* data, size_t size) const override {
    if (offset > v3::kJournalBytes ||
        size > v3::kJournalBytes - offset) return false;
    if (!security_v3_ab_preflight::read(
            flash_, page_, static_cast<uint32_t>(
                v3::kJournalOffset + offset), data, size)) {
      failed_read_ = true;
      return false;
    }
    return true;
  }
  bool failedRead() const { return failed_read_; }
 private:
  const FlashBackend& flash_;
  unsigned page_;
  mutable bool failed_read_ = false;
};

PageResult recoverLegacy(const FlashBackend& flash, unsigned page,
                         Page& candidate) {
  const bool is_v1 = candidate.version == sf::kVersionV1;
  const uint32_t count = is_v1 ? sf::kV1TxReserveSlotsPerPage :
                                   sf::kSecurityStateSlotsPerPage;
  const uint32_t width = is_v1 ? sf::kTxReserveRecordSize :
                                   sf::kSecurityStateRecordSize;
  const uint32_t start = sf::kPageHeaderSize + sf::kCredentialRecordSize;
  bool seen_erased = false;
  uint64_t tx = 0U;
  uint64_t a2d = 0U;
  for (uint32_t slot = 0U; slot < count; ++slot) {
    uint8_t raw[sf::kSecurityStateRecordSize]{};
    if (!read(flash, page, start + slot * width, raw, width))
      return PageResult::kReadFault;
    if (jf::erased(raw, width)) {
      seen_erased = true;
      continue;
    }
    if (seen_erased) return PageResult::kFault;
    if (jf::erased(raw + width - 4U, 4U)) continue;
    if (is_v1) {
      sf::TxReserve r{};
      if (!sf::decodeTxReserve(raw, r) ||
          memcmp(r.credential_id, candidate.credential.credential_id,
                 sf::kCredentialIdSize) != 0 ||
          r.key_epoch != candidate.credential.key_epoch ||
          r.tx_reserved_bound <= tx)
        return PageResult::kFault;
      tx = r.tx_reserved_bound;
    } else {
      sf::SecurityStateRecord r{};
      if (!sf::decodeSecurityState(raw, r) ||
          memcmp(r.credential_id, candidate.credential.credential_id,
                 sf::kCredentialIdSize) != 0 ||
          r.key_epoch != candidate.credential.key_epoch)
        return PageResult::kFault;
      if (r.kind == sf::SecurityStateKind::kTxReserveExclusiveBound) {
        if (r.value < tx) return PageResult::kFault;
        tx = r.value;
      } else if (r.kind ==
                 sf::SecurityStateKind::kA2dReplayExclusiveBound) {
        if (r.value < a2d) return PageResult::kFault;
        a2d = r.value;
      } else {
        return PageResult::kFault;
      }
    }
  }
  const uint32_t tail = start + count * width;
  if (tail < kPageSize) {
    uint8_t erased_tail[sf::kSecurityStateTailBytes]{};
    const size_t length = static_cast<size_t>(kPageSize - tail);
    if (!read(flash, page, tail, erased_tail, length))
      return PageResult::kReadFault;
    if (!jf::erased(erased_tail, length)) return PageResult::kFault;
  }
  candidate.snapshot = v3::Snapshot{};
  memcpy(candidate.snapshot.credential_id,
         candidate.credential.credential_id, sf::kCredentialIdSize);
  candidate.snapshot.key_epoch = candidate.credential.key_epoch;
  candidate.snapshot.tx_bound = tx;
  candidate.snapshot.a2d_bound = a2d;
  return PageResult::kPresent;
}

PageResult inspectPage(const FlashBackend& flash, unsigned page,
                       uint64_t tracker_id, Page& result) {
  uint8_t header[sf::kPageHeaderSize]{};
  if (!read(flash, page, 0U, header, sizeof(header)))
    return PageResult::kReadFault;

  // A genuinely blank page must have no residual credential or journal
  // bytes. If only its header was erased but durable payload remains, it
  // may be a torn erase of the previously newest page. Never silently
  // fall back to an older committed credential or revoked Gateway state.
  if (jf::erased(header, sizeof(header))) {
    uint8_t scratch[v3::kRecordBytes]{};
    for (uint32_t offset = sf::kPageHeaderSize; offset < kPageSize;
         offset += static_cast<uint32_t>(sizeof(scratch))) {
      const uint32_t remaining = kPageSize - offset;
      const size_t length = remaining < sizeof(scratch) ?
                            static_cast<size_t>(remaining) :
                            sizeof(scratch);
      if (!read(flash, page, offset, scratch, length))
        return PageResult::kReadFault;
      if (!jf::erased(scratch, length)) return PageResult::kFault;
    }
    return PageResult::kAbsent;
  }
  uint8_t version = 0xFFU;
  const bool magic = sf::headerMagicPresent(header, &version);
  if (magic && version != 0xFFU &&
      version != sf::kVersionV1 && version != sf::kVersionV2 &&
      version != v3::kVersion)
    return PageResult::kUnsupported;
  if (jf::erased(header + 28U, 4U)) return PageResult::kStaged;
  if (!magic) return PageResult::kFault;

  uint64_t generation = 0U;
  uint64_t owner = 0U;
  if (version == v3::kVersion) {
    if (jf::get32(header + 28U) != 0U ||
        jf::get32(header + 24U) != jf::crc32(header, 24U) ||
        header[5] != 0U || header[6] != 0U || header[7] != 0U)
      return PageResult::kFault;
    generation = jf::get64(header + 8U);
    owner = jf::get64(header + 16U);
    if (generation == 0U) return PageResult::kFault;
  } else if (version == sf::kVersionV1 ||
             version == sf::kVersionV2) {
    sf::PageHeader decoded{};
    if (!sf::decodePageHeaderVersion(header, version, decoded))
      return PageResult::kFault;
    generation = decoded.generation;
    owner = decoded.device_identity;
  } else {
    return PageResult::kFault;
  }

  uint8_t cred_bytes[sf::kCredentialRecordSize]{};
  if (!read(flash, page, sf::kPageHeaderSize,
            cred_bytes, sizeof(cred_bytes)))
    return PageResult::kReadFault;
  sf::Credential cred{};
  if (!sf::decodeCredential(cred_bytes, cred) ||
      cred.device_identity != owner ||
      !validId(cred.credential_id) ||
      cred.key_epoch == UINT32_MAX)
    return PageResult::kFault;
  if (owner != tracker_id) return PageResult::kForeign;

  Page candidate{};
  candidate.version = version;
  candidate.generation = generation;
  candidate.credential = cred;

  PageResult state_result = PageResult::kFault;
  if (version == v3::kVersion) {
    const JournalFlashView reader(flash, page);
    if (v3::recoverCommittedJournalStream(
            reader, cred.credential_id, cred.key_epoch,
            candidate.snapshot)) {
      state_result = PageResult::kPresent;
    } else {
      state_result = reader.failedRead() ? PageResult::kReadFault :
                                          PageResult::kFault;
    }
  } else {
    state_result = recoverLegacy(flash, page, candidate);
  }
  if (state_result != PageResult::kPresent) return state_result;
  candidate.present = true;
  result = candidate;
  return PageResult::kPresent;
}

uint32_t maxGeneration(const v3::Snapshot& state) {
  uint32_t result = 0U;
  for (size_t i = 0U; i < v3::kGatewayCount; ++i)
    if (state.gateways[i].generation > result)
      result = state.gateways[i].generation;
  return result;
}

bool samePolicy(const v3::Snapshot& a, const v3::Snapshot& b) {
  if (a.policy_revision != b.policy_revision ||
      a.custody_floor != b.custody_floor) return false;
  for (size_t i = 0U; i < v3::kGatewayCount; ++i) {
    if (a.gateways[i].gateway_id != b.gateways[i].gateway_id ||
        a.gateways[i].generation != b.gateways[i].generation)
      return false;
  }
  return true;
}

bool sameCredential(const Page& a, const Page& b) {
  return memcmp(a.credential.credential_id,
                b.credential.credential_id, sf::kCredentialIdSize) == 0;
}

bool sameRoot(const Page& a, const Page& b) {
  return memcmp(a.credential.k_root,
                b.credential.k_root, sf::kKRootSize) == 0;
}

bool compatibleSuccessor(const Page& old_page, const Page& next) {
  // v3 -> v2 or v3 -> v1 can NEVER be a normal v3 compaction/migration:
  // even with a higher header generation it drops custody authority state.
  if (old_page.version == v3::kVersion &&
      next.version != v3::kVersion)
    return false;
  const bool matching_id = sameCredential(old_page, next);
  const bool matching_root = sameRoot(old_page, next);
  if (matching_id != matching_root) return false;
  if (!matching_id) {
    // Changing both credential ID and root is an independent namespace.
    // Old Gateway grants must not survive into the new credential.
    const v3::Snapshot empty{};
    return samePolicy(next.snapshot, empty);
  }
  if (old_page.credential.key_epoch != next.credential.key_epoch ||
      next.snapshot.tx_bound < old_page.snapshot.tx_bound ||
      next.snapshot.a2d_bound < old_page.snapshot.a2d_bound ||
      next.snapshot.policy_revision < old_page.snapshot.policy_revision ||
      next.snapshot.custody_floor < old_page.snapshot.custody_floor ||
      maxGeneration(next.snapshot) < maxGeneration(old_page.snapshot))
    return false;

  if (next.snapshot.policy_revision == old_page.snapshot.policy_revision)
    return samePolicy(old_page.snapshot, next.snapshot);

  // A larger global revision can conceal a lowered per-slot tombstone if
  // another slot gains a newer generation. Preserve EACH slot high-water,
  // and require an identity change in a slot to advance that slot's gen.
  for (size_t i = 0U; i < v3::kGatewayCount; ++i) {
    const v3::GatewaySlot& previous = old_page.snapshot.gateways[i];
    const v3::GatewaySlot& selected = next.snapshot.gateways[i];
    if (selected.generation < previous.generation ||
        (selected.generation == previous.generation &&
         selected.gateway_id != previous.gateway_id))
      return false;
  }
  return true;
}

}  // namespace

Result inspect(const FlashBackend& flash, uint64_t tracker_id,
               Summary& out) {
  if (tracker_id == 0U) return Result::kFault;
  Page found[kPages]{};
  PageResult classification[kPages]{};
  for (unsigned page = 0U; page < kPages; ++page) {
    classification[page] = inspectPage(flash, page, tracker_id, found[page]);
    if (classification[page] == PageResult::kReadFault)
      return Result::kReadFault;
    if (classification[page] == PageResult::kUnsupported)
      return Result::kUnsupported;
    if (classification[page] == PageResult::kFault)
      return Result::kFault;
    if (classification[page] == PageResult::kForeign)
      return Result::kForeign;
  }

  int winner = -1;
  for (unsigned page = 0U; page < kPages; ++page) {
    if (!found[page].present) continue;
    if (winner == -1 ||
        found[page].generation >
            found[static_cast<unsigned>(winner)].generation) {
      winner = static_cast<int>(page);
    } else if (found[page].generation ==
               found[static_cast<unsigned>(winner)].generation) {
      return Result::kFault;
    }
  }
  if (winner == -1) return Result::kUnprovisioned;
  const unsigned selected = static_cast<unsigned>(winner);
  const unsigned other = 1U - selected;
  const Page& page = found[selected];
  if (found[other].present &&
      !compatibleSuccessor(found[other], page))
    return Result::kFault;

  Summary summary{};
  summary.selected_page = static_cast<uint8_t>(selected);
  summary.selected_version = page.version;
  summary.generation = page.generation;
  summary.tx_reserved_bound = page.snapshot.tx_bound;
  summary.a2d_replay_bound = page.snapshot.a2d_bound;
  summary.policy_revision = page.snapshot.policy_revision;
  summary.custody_floor = page.snapshot.custody_floor;
  summary.max_grant_generation = maxGeneration(page.snapshot);
  for (size_t i = 0U; i < v3::kGatewayCount; ++i)
    if (page.snapshot.gateways[i].gateway_id != 0U)
      ++summary.active_gateway_count;
  out = summary;

  return page.version == v3::kVersion ? Result::kV3 :
         page.version == sf::kVersionV1 ? Result::kV1 : Result::kV2;
}

}  // namespace security_v3_ab_preflight
}  // namespace orun_tlp
