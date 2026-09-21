// M7P7D: typed application request seam. Uses the production ConfigStore with
// a read-only fake flash so the test proves the seam reads the existing owner
// without creating a second config store or mutating persistence.
#include <assert.h>
#include <string.h>

#include <array>

#include "application_request.h"
#include "config_store.h"
#include "storage_config.h"

using namespace orun_tlp;

namespace {

constexpr size_t kRegionSize =
    storage_config::kPageSize * storage_config::kFutureConfigRegionPages;

class ReadOnlyFlash : public FlashBackend {
 public:
  std::array<uint8_t, kRegionSize> bytes{};
  bool fail_begin = false;
  unsigned program_calls = 0;
  unsigned erase_calls = 0;

  ReadOnlyFlash() { bytes.fill(0xFF); }

  bool begin() override { return !fail_begin; }

  bool read(uint32_t offset, void* data, size_t size) const override {
    if (data == nullptr || offset > bytes.size() ||
        size > bytes.size() - offset) {
      return false;
    }
    memcpy(data, bytes.data() + offset, size);
    return true;
  }

  FlashOpResult program(uint32_t, const void*, size_t) override {
    ++program_calls;
    return FlashOpResult::kFailed;
  }

  FlashOpResult erasePage(uint32_t) override {
    ++erase_calls;
    return FlashOpResult::kFailed;
  }

  void seedConfig(const config_format::Config& config,
                  uint64_t generation = 1,
                  unsigned page = 0) {
    assert(page < storage_config::kFutureConfigRegionPages);
    uint8_t record[config_format::kRecordSize]{};
    config_format::encode(config, generation, record);
    memcpy(bytes.data() + page * storage_config::kPageSize, record,
           sizeof(record));
  }
};

ApplicationResponse take(ApplicationRequestService& service) {
  ApplicationResponse response;
  assert(service.takeResponse(response));
  assert(!service.responsePending());
  return response;
}

}  // namespace

int main() {
  // 1. Blank durable config: the application seam returns ConfigStore's
  // existing safe defaults and says the durable owner is ready.
  {
    ReadOnlyFlash flash;
    ConfigStore store(flash);
    assert(store.begin());
    ApplicationRequestService service(store);

    assert(service.submit(
               ApplicationRequest{7, ApplicationRequestKind::kGetConfig}) ==
           ApplicationSubmitResult::kAccepted);
    const ApplicationResponse response = take(service);
    assert(response.request_id == 7);
    assert(response.code == ApplicationResponseCode::kOk);
    assert(response.config_store_ready);
    assert(response.config.tracking_interval_seconds == 180);
    assert(response.config.battery_capacity_mah == 0);
    assert(flash.program_calls == 0);
    assert(flash.erase_calls == 0);
  }

  // 2. A recovered non-default config comes from the production ConfigStore,
  // not a duplicated application-side cache.
  {
    ReadOnlyFlash flash;
    flash.seedConfig(config_format::Config{247, 9000}, 3);
    ConfigStore store(flash);
    assert(store.begin());
    ApplicationRequestService service(store);

    assert(service.submit(
               ApplicationRequest{42, ApplicationRequestKind::kGetConfig}) ==
           ApplicationSubmitResult::kAccepted);
    const ApplicationResponse response = take(service);
    assert(response.request_id == 42);
    assert(response.code == ApplicationResponseCode::kOk);
    assert(response.config_store_ready);
    assert(response.config.tracking_interval_seconds == 247);
    assert(response.config.battery_capacity_mah == 9000);
    assert(flash.program_calls == 0);
    assert(flash.erase_calls == 0);
  }

  // 3. One result slot is hard backpressure. A second request cannot replace
  // an unread response and therefore cannot confuse request/result ownership.
  {
    ReadOnlyFlash flash;
    ConfigStore store(flash);
    assert(store.begin());
    ApplicationRequestService service(store);

    assert(service.submit(
               ApplicationRequest{100, ApplicationRequestKind::kGetConfig}) ==
           ApplicationSubmitResult::kAccepted);
    assert(service.responsePending());
    assert(service.submit(
               ApplicationRequest{101, ApplicationRequestKind::kGetConfig}) ==
           ApplicationSubmitResult::kBusy);

    const ApplicationResponse first = take(service);
    assert(first.request_id == 100);

    assert(service.submit(
               ApplicationRequest{101, ApplicationRequestKind::kGetConfig}) ==
           ApplicationSubmitResult::kAccepted);
    const ApplicationResponse second = take(service);
    assert(second.request_id == 101);
  }

  // 4. Unknown typed operations fail closed and perform no flash mutation.
  {
    ReadOnlyFlash flash;
    ConfigStore store(flash);
    assert(store.begin());
    ApplicationRequestService service(store);

    const auto unknown = static_cast<ApplicationRequestKind>(0xFE);
    assert(service.submit(ApplicationRequest{200, unknown}) ==
           ApplicationSubmitResult::kAccepted);
    const ApplicationResponse response = take(service);
    assert(response.request_id == 200);
    assert(response.code == ApplicationResponseCode::kUnsupported);
    assert(!response.config_store_ready);
    assert(flash.program_calls == 0);
    assert(flash.erase_calls == 0);
  }

  // 5. ConfigStore's documented safe fallback remains distinguishable from a
  // recovered durable value when its backend cannot initialize.
  {
    ReadOnlyFlash flash;
    flash.fail_begin = true;
    ConfigStore store(flash);
    assert(!store.begin());
    ApplicationRequestService service(store);

    assert(service.submit(
               ApplicationRequest{300, ApplicationRequestKind::kGetConfig}) ==
           ApplicationSubmitResult::kAccepted);
    const ApplicationResponse response = take(service);
    assert(response.code == ApplicationResponseCode::kOk);
    assert(!response.config_store_ready);
    assert(response.config.tracking_interval_seconds == 180);
    assert(response.config.battery_capacity_mah == 0);
    assert(flash.program_calls == 0);
    assert(flash.erase_calls == 0);
  }

  return 0;
}
