#include "kq05_supplemental_28k_census.h"

#include <string.h>

namespace orun_tlp {
namespace kq05_supplemental {
namespace {

namespace obs = observation_store_format;
namespace custody = custody_store_product_format;

bool allErased(const uint8_t* bytes, size_t length) {
  for (size_t i = 0U; i < length; ++i)
    if (bytes[i] != 0xFFU) return false;
  return true;
}

bool hasMagic(const uint8_t* data, const char* magic) {
  return memcmp(data, magic, 4U) == 0;
}

Result failure(Result out, State state, uint8_t page) {
  out.state = state;
  out.first_bad_page = page;
  return out;
}

}  // namespace

Result inspectReadOnly(const FlashBackend& history_port, Role local_role,
                       uint64_t local_device_id) {
  Result out{};
  if ((local_role != Role::kTracker && local_role != Role::kGateway) ||
      local_device_id == 0U || local_device_id == UINT64_MAX) {
    out.state = State::kBlockedRole;
    return out;
  }

  // Inspect 100% of all 7 pages. A completely erased *header* is not
  // evidence of a blank page if a previous writer left data in its body.
  uint8_t chunk[64U]{};
  bool saw_v3_unbound = false;
  bool saw_v4_bound = false;
  for (uint32_t p = 0U; p < kPages; ++p) {
    const uint32_t page_base = p * kPageBytes;
    bool erased_page = true;
    for (uint32_t off = 0U; off < kPageBytes; off += sizeof(chunk)) {
      if (!history_port.read(page_base + off, chunk, sizeof(chunk)))
        return failure(out, State::kReadFailed, static_cast<uint8_t>(p));
      if (!allErased(chunk, sizeof(chunk))) erased_page = false;
    }
    if (erased_page) {
      ++out.erased_pages;
      continue;
    }

    if (!history_port.read(page_base, chunk, sizeof(chunk)))
      return failure(out, State::kReadFailed, static_cast<uint8_t>(p));

    if (local_role == Role::kTracker) {
      if (!hasMagic(chunk, "OBS1")) {
        return failure(out, State::kLegacyOrForeign,
                       static_cast<uint8_t>(p));
      }
      obs::PageInspection header{};
      if (!obs::inspectPageHeader(chunk, sizeof(chunk), header))
        return failure(out, State::kCorruptOrIncomplete,
                       static_cast<uint8_t>(p));
      if (header.evidence != obs::PageEvidence::kActive &&
          header.evidence != obs::PageEvidence::kPrepared) {
        return failure(out, State::kCorruptOrIncomplete,
                       static_cast<uint8_t>(p));
      }
      if (header.device_id != local_device_id) {
        return failure(out, State::kLegacyOrForeign,
                       static_cast<uint8_t>(p));
      }
    } else {
      if (!hasMagic(chunk, "OCQ2")) {
        return failure(out, State::kLegacyOrForeign,
                       static_cast<uint8_t>(p));
      }
      custody::PageInspection header{};
      if (!custody::inspectPageHeader(chunk, sizeof(chunk), header))
        return failure(out, State::kCorruptOrIncomplete,
                       static_cast<uint8_t>(p));
      if (header.evidence != custody::PageEvidence::kActive &&
          header.evidence != custody::PageEvidence::kPrepared) {
        return failure(out, State::kCorruptOrIncomplete,
                       static_cast<uint8_t>(p));
      }
      if (header.gateway_bound) {
        if (header.gateway_id != local_device_id) {
          return failure(out, State::kLegacyOrForeign,
                         static_cast<uint8_t>(p));
        }
        saw_v4_bound = true;
      } else {
        // A v3 OCQ2 header cannot assert Gateway identity. It is never
        // sufficient to mount a supplemental store or mint an ACK.
        saw_v3_unbound = true;
      }
      if (saw_v3_unbound && saw_v4_bound) {
        // No silent mixed-format adoption/migration in the old region.
        return failure(out, State::kLegacyOrForeign,
                       static_cast<uint8_t>(p));
      }
    }
    ++out.candidate_pages;
  }

  if (out.candidate_pages == 0U) out.state = State::kBlank;
  else if (local_role == Role::kTracker)
    out.state = State::kPotentialTracker;
  else if (saw_v4_bound)
    out.state = State::kGatewayBoundCandidate;
  else out.state = State::kGatewayNeedsBinding;
  return out;
}

}  // namespace kq05_supplemental
}  // namespace orun_tlp
