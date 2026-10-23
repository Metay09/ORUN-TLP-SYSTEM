#pragma once

// HOST-ONLY adapter that makes SF5K3A exercise the actual ObservationStore.
// Exact bytes are *test-provided*, NOT an authenticated ACK or permission to
// release production records. There is no runtime registration/entrypoint.
#include <stddef.h>
#include "observation_store.h"
#include "sf5k3_release_progress_candidate.h"

namespace orun_tlp {
namespace sf5k3_candidate {

class ObservationStorePort final : public StorePort {
 public:
  ObservationStorePort(ObservationStore& store,
                       const osc::ExactObject* expected, size_t count);
  bool ready() const { return ready_; }
  bool inspect(const osf::RecordIdentity& id, DurableView& out) override;
  Request requestRelease(const osf::RecordIdentity& id) override;
  bool takeReleaseResult(bool& success) override;

 private:
  const osc::ExactObject* expectedFor(const osf::RecordIdentity& id) const;
  ObservationStore& store_;
  osc::ExactObject expected_[kMaxMembers]{};
  size_t count_ = 0U;
  bool ready_ = false;
};

} // namespace sf5k3_candidate
} // namespace orun_tlp
