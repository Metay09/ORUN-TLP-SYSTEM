#include "sf5k2_gateway_batch_candidate.h"
#include <string.h>

namespace orun_tlp {
namespace sf5k2_candidate {
namespace {
uint64_t read64(const uint8_t* p) {
  uint64_t v = 0U;
  for (size_t i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}
uint32_t read32(const uint8_t* p) {
  uint32_t v = 0U;
  for (size_t i = 0; i < 4; ++i) v = (v << 8) | p[i];
  return v;
}
bool same(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t mismatch = 0U;
  for (size_t i = 0U; i < n; ++i)
    mismatch |= static_cast<uint8_t>(a[i] ^ b[i]);
  return mismatch == 0U;
}
bool admissible(const Scope& s, const Pending& p) {
  if (p.exact == nullptr || !rec::productObjectShape(p.exact, p.size) ||
      (p.exact[4] & 0x02U) == 0U ||
      read64(p.exact + 8U) != s.tracker_id ||
      read32(p.exact + 16U) != s.key_epoch) return false;
  return true;
}
} // namespace

Result prepare(const Scope& scope, const Pending* objects, size_t count,
               EvidencePort& evidence, PreparedCandidate* out) {
  if (!out || !objects || count == 0U || count > kWindow ||
      scope.tracker_id == 0U || scope.gateway_id == 0U ||
      scope.key_epoch == 0U || scope.key_epoch == UINT32_MAX ||
      scope.grant_generation == 0U) return Result::kInvalid;

  // Phase one: reject *all* malformed/mixed identities/duplicate frames
  // BEFORE calling an evidence port, so none of those requests get staging.
  for (size_t i = 0U; i < count; ++i) {
    if (!admissible(scope, objects[i])) return Result::kInvalid;
    for (size_t j = 0U; j < i; ++j) {
      if (objects[i].size == objects[j].size &&
          same(objects[i].exact, objects[j].exact, objects[i].size))
        return Result::kInvalid;
    }
  }

  PreparedCandidate candidate{};
  candidate.scope = scope;
  for (size_t i = 0U; i < count; ++i) {
    const Pending& p = objects[i];
    uint8_t flash[rec::kRecordSize]{};
    bool durable = false;
    if (evidence.readLocalCandidate(i, flash)) {
      rec::RecordInspection inspected{};
      if (rec::inspectRecord(flash, sizeof(flash), inspected) &&
          rec::sameExactObject(inspected, p.exact, p.size)) {
        // Only a completed record with verified CRC, activation marker and
        // exact bytes qualifies. Partial/torn/erased local evidence does not.
        durable = true;
      }
    }
    // Started local fallback is never cancelled on late Edge response.
    // A fully read-back committed local record remains independently valid.
    if (!durable && !p.local_mutation_started) {
      durable = evidence.edgeFact(i, scope, p.exact, p.size) ==
                EdgeFact::kAuthenticatedDurableExact;
    }
    if (!durable) continue;
    ExactMember& m = candidate.members[candidate.count];
    m.size = static_cast<uint8_t>(p.size);
    m.input_index = static_cast<uint8_t>(i);
    memcpy(m.bytes, p.exact, p.size);
    ++candidate.count;
  }
  if (candidate.count == 0U) return Result::kNoDurableFact;
  // Staging only: no SHA256/ref generation, MAC, signed 0x06 or RF emission.
  *out = candidate;
  return Result::kPrepared;
}

} // namespace sf5k2_candidate
} // namespace orun_tlp
