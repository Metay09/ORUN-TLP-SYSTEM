#include "product_region.h"

#include "custody_store_format.h"
#include "observation_store_format.h"

namespace orun_tlp::product_region {
namespace {

constexpr uint32_t kPageSize = observation_store_format::kPageSize;
constexpr size_t kChunkSize = 64;

static_assert(custody_store_format::kPageSize == kPageSize,
              "both product stores use the same page size");
static_assert(observation_store_format::kPageMagic !=
                  custody_store_format::kPageMagic,
              "the two product stores must be distinguishable");

uint32_t getBe32(const uint8_t* data) {
  return (uint32_t(data[0]) << 24U) | (uint32_t(data[1]) << 16U) |
         (uint32_t(data[2]) << 8U) | uint32_t(data[3]);
}

}  // namespace

Owner ownerFor(const Jobs& jobs) {
  if (jobs.gateway) return Owner::kCustody;
  if (jobs.tracking) return Owner::kObservation;
  return Owner::kNone;
}

const char* ownerName(Owner owner) {
  switch (owner) {
    case Owner::kNone:
      return "NONE";
    case Owner::kObservation:
      return "OBSERVATION";
    case Owner::kCustody:
      return "CUSTODY";
  }
  return "NONE";
}

bool takeCensus(const FlashBackend& flash, uint16_t page_count,
                Census& census) {
  census = Census{0U, 0U, 0U, 0U};
  uint8_t chunk[kChunkSize];
  for (uint16_t page = 0; page < page_count; ++page) {
    const uint32_t base = uint32_t(page) * kPageSize;
    if (!flash.read(base, chunk, sizeof(chunk))) return false;
    const uint32_t magic = getBe32(chunk);
    if (magic == observation_store_format::kPageMagic) {
      ++census.observation;
      continue;
    }
    if (magic == custody_store_format::kPageMagic) {
      ++census.custody;
      continue;
    }

    bool erased = true;
    for (uint32_t offset = 0; erased && offset < kPageSize;
         offset += kChunkSize) {
      if (offset != 0U && !flash.read(base + offset, chunk, sizeof(chunk)))
        return false;
      for (size_t i = 0; i < sizeof(chunk); ++i)
        if (chunk[i] != 0xFFU) {
          erased = false;
          break;
        }
    }
    if (erased)
      ++census.erased;
    else
      ++census.unknown;
  }
  return true;
}

MountDecision mountDecision(Owner owner, const Census& census) {
  if (owner == Owner::kNone) return MountDecision::kNoOwner;
  const uint16_t own = owner == Owner::kObservation ? census.observation
                                                    : census.custody;
  const uint16_t foreign = owner == Owner::kObservation ? census.custody
                                                        : census.observation;
  if (foreign != 0U) return MountDecision::kForeignStore;
  if (own == 0U && census.unknown != 0U)
    return MountDecision::kNeedsPreparation;
  return MountDecision::kMount;
}

const char* mountDecisionName(MountDecision decision) {
  switch (decision) {
    case MountDecision::kMount:
      return "MOUNT";
    case MountDecision::kNoOwner:
      return "NO_OWNER";
    case MountDecision::kForeignStore:
      return "FOREIGN_STORE";
    case MountDecision::kNeedsPreparation:
      return "NEEDS_PREPARATION";
  }
  return "NO_OWNER";
}

}  // namespace orun_tlp::product_region
