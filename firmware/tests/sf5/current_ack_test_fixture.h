#pragma once

#include <assert.h>
#include <string.h>
#include <vector>

#include "gateway_custody_authorized_ingress.h"
#include "kq05_tracker_batch_ack_verifier.h"
#include "product_record_store.h"
#include "tlp_v2_product_secure.h"

namespace orun_tlp {

namespace current_ack_test {

struct Nor final : FlashBackend {
  std::vector<uint8_t> bytes;

  Nor() : bytes(32U * 4096U, 0xFFU) {}

  bool begin() override { return true; }

  bool read(uint32_t off, void* out, size_t size) const override {
    if (!out || off > bytes.size() ||
        size > bytes.size() - off)
      return false;

    memcpy(out, bytes.data() + off, size);
    return true;
  }

  FlashOpResult program(uint32_t off, const void* data,
                        size_t size) override {
    if (!data || !size || (off & 3U) || (size & 3U) ||
        off > bytes.size() || size > bytes.size() - off)
      return FlashOpResult::kFailed;

    const auto* p = static_cast<const uint8_t*>(data);

    for (size_t i = 0; i < size; ++i)
      if (bytes[off + i] != 0xFFU)
        return FlashOpResult::kFailed;

    for (size_t i = 0; i < size; ++i)
      bytes[off + i] &= p[i];

    return FlashOpResult::kDone;
  }

  FlashOpResult erasePage(uint32_t page) override {
    if (page >= 32U) return FlashOpResult::kFailed;

    memset(bytes.data() + page * 4096U, 0xFFU, 4096U);

    return FlashOpResult::kDone;
  }
};

struct Source final : ObservationIncarnationSource {
  bool generate(uint64_t& out) override {
    out = 77U;
    return true;
  }
};

inline bool takeSingle(
    GatewayCustodyAuthorizedIngress& ingress,
    uint8_t (&ack)[kq05_gateway_batch_ack::kMinSize]) {

  ProductCustodyStore::Handle handle{};
  GatewayCustodyAckScope scope{};

  if (!ingress.takeAuthorizedBatchHandle(handle, scope))
    return false;

  uint8_t wire[kq05_gateway_batch_ack::kMaxSize]{};
  size_t length = 0U;

  if (!ingress.prepareAuthorizedBatchAck(
          &handle, 1U, scope, wire, length) ||
      length != sizeof(ack))
    return false;

  memcpy(ack, wire, length);
  return true;
}

struct Peer {
  static Kq05TrackerBatchVerifyResult check(
      Kq05TrackerBatchAckVerifier& verifier,
      const uint8_t* wire, size_t length,
      const observation_store_control::ExactObject* cached,
      uint16_t count) {

    using namespace observation_store_format;

    assert(cached && count == 1U);

    tlp::ProductSecureFrame frame{};

    assert(tlp::decodeProductSecureFrame(
        cached[0].object, cached[0].object_size, &frame) ==
        tlp::ProductSecureCodecResult::kOk);

    Nor nor;
    Source source;

    ProductRecordStore records(nor, 32U, &source);

    records.begin(
        frame.device_id,
        product_region::Jobs{false, true});

    assert(records.mounted());

    PeriodicRecordInputs p{};
    p.period_duration_seconds = 3600U;
    p.location.valid = true;
    p.location.latitude_e7 = 376102718;
    p.location.longitude_e7 = 280545028;
    p.location.hdop_x100 = 120U;
    p.location.satellites = 7U;
    p.location.age_valid = true;
    p.location.age_seconds = 3U;
    p.battery_valid = true;
    p.battery_mv = 4100U;
    p.battery_state = 1U;
    p.health.valid = true;

    assert(records.submitPeriodic(p) ==
           ProductRecordStore::Submit::kAccepted);

    ProductRecordStore::Outcome outcome{};

    bool completed = false;

    for (unsigned i = 0; i < 20000U; ++i) {
      records.poll();

      if (records.takeOutcome(outcome)) {
        completed = true;
        break;
      }
    }

    assert(completed && outcome.stored);

    auto exact = cached[0];

    exact.identity.incarnation = outcome.incarnation;
    exact.identity.sequence = outcome.sequence;

    assert(records.submitExactCache(exact) ==
           ProductRecordStore::CacheSubmit::kStarted);

    ProductRecordStore::CacheOutcome cache{};

    completed = false;

    for (unsigned i = 0; i < 30000U; ++i) {
      records.poll();

      if (records.takeCacheOutcome(cache)) {
        completed = true;
        break;
      }
    }

    assert(completed && cache.durable);

    AuthenticatedKq05TrackerBatchAck proof{};

    return verifier.verifyFromStore(
        wire, length, records, proof);
  }
};

}  // namespace current_ack_test
}  // namespace orun_tlp
