#pragma once
// SF5K3D: HOST-ONLY A/B snapshot & migration contract. NOT a flash writer.
#include <stddef.h>
#include <stdint.h>
#include "sf5k3c_security_v3_candidate.h"

namespace orun_tlp {
namespace sf5k3d_candidate {
namespace c = sf5k3c_candidate;

constexpr size_t kSnapshotRecords = 1U + c::kMaxGateways;  // 5
constexpr size_t kEntryAreaSize =
    c::kSlotCount * c::kEntrySize + c::kTailBytes;
constexpr size_t kJournalOffset = c::kHeaderSize + c::kCredentialSize;

static_assert(kJournalOffset + kEntryAreaSize == c::kPageSize,
              "v3 snapshot must fit existing two-page region");
static_assert(kSnapshotRecords + 2U <= c::kSlotCount,
              "initial snapshot, TX and replay exclusive bounds must fit");

// Required first five v3 journal entries: meta (revision, floor, max gen),
// then ALL four explicit Gateway slots including never-used and tombstones.
// Returns no partially emitted entries on failure.
bool encodeSnapshot(const c::State& state,
                    uint8_t (&records)[kSnapshotRecords][c::kEntrySize]);

// Full journal recovery: demands all five committed snapshot records at
// slots 0..4, then replays only genuine authenticated-state journal entries
// from slot 5 onward; checks blank reserved tail, holes, CRC and key scope.
// Uses only *physical* committed state, not caller-supplied authorization.
// Does not authorize any ACK/release.
bool recoverSnapshotJournal(const uint8_t* region, size_t region_size,
                            const uint8_t (&credential_id)[16],
                            uint32_t epoch, c::State& result);

enum class BootResult : uint8_t {
  kUnprovisioned, kV2, kV3, kUnsupported, kFault
};

// Read-only two-page boot selection model: v2 uses the ACTUAL
// security_format codec; v3 uses the candidate snapshot decoder. As in
// SecurityStore, any ambiguous committed corruption fails closed and an
// unsupported future version blocks downgrade. Staged pages have no
// activation marker and cannot supersede an older committed page.
BootResult recoverTwoPages(const uint8_t (&pages)[2][c::kPageSize],
                           uint64_t tracker_id, c::State& out,
                           uint64_t& selected_generation);

// Create an entire inert proposed v3 page from a source state.
// The header/record COMMITs are serialized in `image`, but there is no
// real flash operation. A caller must write/verify all entry bodies/commits
// and the credential before writing the header activation word last.
bool makeV3SnapshotPage(uint64_t tracker_id, uint64_t page_generation,
                        const c::State& state, const uint8_t (&root)[32],
                        uint8_t (&image)[c::kPageSize]);

}  // namespace sf5k3d_candidate
}  // namespace orun_tlp
