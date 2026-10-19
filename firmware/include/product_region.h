#pragma once

#include <stddef.h>
#include <stdint.h>

#include "flash_backend.h"

namespace orun_tlp::product_region {

// Owner decision D2 (docs/milestones/SF5D.md, 2026-10-12): the whole 128 KiB
// product region belongs to ONE store per device, chosen by the device's job.
//
// - A device with the Gateway job gives it to CustodyStore, also when it
//   could track: storing what it receives must not fail, and a gateway keeps
//   no durable records of its own (a fixed gateway's location is configured,
//   a mobile gateway's comes from the phone app).
// - Any other device that originates observations (tracking) gives it to
//   ObservationStore.
// - Relay forwarding alone is not custody and owns nothing.
enum class Owner : uint8_t { kNone, kObservation, kCustody };

// The jobs that decide the owner. The runtime fills these from the effective
// services. Legacy BASE receive is NOT the Gateway job (AGENTS.md: today's
// BASE only receives legacy traffic), so `gateway` stays false until the
// Gateway service exists. Plain aggregates (no default member initializers):
// the firmware builds as gnu++11, where those would stop brace initialisation.
struct Jobs {
  bool gateway;
  bool tracking;
};

Owner ownerFor(const Jobs& jobs);
const char* ownerName(Owner owner);

// What a page holds, from its first header word: the product stores begin
// every page with their own magic. Anything else that is not fully erased
// (leftover application code, test markers) is unknown.
enum class PageContent : uint8_t { kErased, kObservation, kCustody, kUnknown, kProductCustody };

struct Census {
  uint16_t erased;
  uint16_t observation;
  uint16_t custody;
  uint16_t unknown;
  uint16_t product_custody;  // OCQ2/v3, never alias legacy OCQ1
};

// Reads every page in small chunks; false on any read failure.
bool takeCensus(const FlashBackend& flash, uint16_t page_count,
                Census& census);

enum class MountDecision : uint8_t {
  // Erased, or holds this owner's pages (the store's own recovery decides
  // the rest and fails closed on anything it does not trust).
  kMount,
  // This device has no job that owns the region.
  kNoOwner,
  // Pages of the other store: refuse; only an explicit, operator-visible
  // maintenance erase may change the owner (blocked while Gateway custody
  // is unresolved). Never reformat implicitly.
  kForeignStore,
  // No store pages, but unrecognised content: refuse until the region is
  // explicitly prepared (erased).
  kNeedsPreparation,
};

MountDecision mountDecision(Owner owner, const Census& census);
// New PRODUCT_SECURE Gateway store: explicit, non-migrating OCQ2 decision.
MountDecision mountProductCustodyDecision(const Census& census);
const char* mountDecisionName(MountDecision decision);

}  // namespace orun_tlp::product_region
