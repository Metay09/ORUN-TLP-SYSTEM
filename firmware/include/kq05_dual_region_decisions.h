#pragma once
#include <stddef.h>
#include <stdint.h>

// KQ05 portable, PC-tested DECISIONS ONLY. No Flash owner, no ACK minting,
// no payload write, no radio, no Tracker record release. All inputs must
// come from separately authenticated, durable owner proofs in future code.
namespace orun_tlp {
namespace kq05_dual_region {

enum class Role : uint8_t { kUnassigned, kTracker, kGateway, kConflict };
enum class Region : uint8_t { kNone, kPrimary128K, kSupplemental28K };
enum class Store : uint8_t { kReady, kFull, kBusy, kUnavailable, kFault };
enum class Decision : uint8_t {
  kPrimary, kSupplemental, kAlreadyPresent, kBusy,
  kFull, kBlocked, kFault, kConflict
};
enum class Proof : uint8_t { kAbsent, kExactDurable, kUncertain, kDifferent };

struct AppendInput {
  Role role = Role::kUnassigned;
  Store primary = Store::kUnavailable;
  Store supplemental = Store::kUnavailable;
  Proof primary_duplicate = Proof::kUncertain;
  Proof supplemental_duplicate = Proof::kUncertain;
  // Facts derived from a trusted device-local provisioning/Flash-owner path,
  // NOT caller-reported packet bits:
  bool legacy_writer_disabled = false;
  bool supplemental_identity_bound = false;
  bool shared_tracker_sequence_owner = false;
};

struct AppendResult {
  Decision decision = Decision::kBlocked;
  Region region = Region::kNone;
};

// Prevent a new write when exact-identity duplicate checks in EITHER region
// are ambiguous. This is particularly important for cross-Gateway retries.
inline AppendResult selectAppend(const AppendInput& in) {
  if (in.role != Role::kTracker && in.role != Role::kGateway)
    return {Decision::kBlocked, Region::kNone};
  if (in.primary == Store::kFault ||
      in.primary_duplicate == Proof::kDifferent ||
      in.supplemental_duplicate == Proof::kDifferent)
    return {Decision::kConflict, Region::kNone};
  if (in.primary_duplicate == Proof::kUncertain ||
      in.supplemental_duplicate == Proof::kUncertain)
    return {Decision::kBlocked, Region::kNone};
  if (in.primary_duplicate == Proof::kExactDurable ||
      in.supplemental_duplicate == Proof::kExactDurable)
    return {Decision::kAlreadyPresent, Region::kNone};
  if (in.primary == Store::kUnavailable || in.primary == Store::kBusy)
    return {Decision::kBusy, Region::kNone};
  if (in.primary == Store::kReady)
    return {Decision::kPrimary, Region::kPrimary128K};
  if (in.primary != Store::kFull)
    return {Decision::kFault, Region::kNone};
  if (!in.legacy_writer_disabled || !in.supplemental_identity_bound ||
      (in.role == Role::kTracker && !in.shared_tracker_sequence_owner))
    return {Decision::kBlocked, Region::kNone};
  switch (in.supplemental) {
    case Store::kReady:
      return {Decision::kSupplemental, Region::kSupplemental28K};
    case Store::kFull:
      return {Decision::kFull, Region::kNone};
    case Store::kBusy:
      return {Decision::kBusy, Region::kNone};
    case Store::kUnavailable:
      return {Decision::kBlocked, Region::kNone};
    case Store::kFault:
      return {Decision::kFault, Region::kNone};
  }
  return {Decision::kFault, Region::kNone};
}

// A single-region release requires authenticated *Gateway custody* proof
// (Tracker) or authenticated *downstream durable acceptance* (Gateway).
// This function does not perform authentication nor modify either store.
struct ReleaseInput {
  Role role = Role::kUnassigned;
  Proof primary = Proof::kUncertain;
  Proof supplemental = Proof::kUncertain;
  bool external_proof_authenticated = false;
  bool scope_matches = false;
};
inline AppendResult selectRelease(const ReleaseInput& in) {
  if ((in.role != Role::kTracker && in.role != Role::kGateway) ||
      !in.external_proof_authenticated || !in.scope_matches)
    return {Decision::kBlocked, Region::kNone};
  if (in.primary == Proof::kUncertain ||
      in.supplemental == Proof::kUncertain)
    return {Decision::kBlocked, Region::kNone};
  if (in.primary == Proof::kDifferent ||
      in.supplemental == Proof::kDifferent)
    return {Decision::kConflict, Region::kNone};
  const bool a = in.primary == Proof::kExactDurable;
  const bool b = in.supplemental == Proof::kExactDurable;
  if (a && b) return {Decision::kConflict, Region::kNone};
  if (a) return {Decision::kPrimary, Region::kPrimary128K};
  if (b) return {Decision::kSupplemental, Region::kSupplemental28K};
  return {Decision::kBlocked, Region::kNone};
}

}  // namespace kq05_dual_region
}  // namespace orun_tlp
