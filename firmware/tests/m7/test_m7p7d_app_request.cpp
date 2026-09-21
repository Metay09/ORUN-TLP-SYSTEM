// M7P7D/M7P7E: typed application request seam and requester ownership. Uses
// the production ConfigStore with a read-only fake flash so the test proves
// the seam reads the existing owner without creating a second config store or
// mutating persistence.
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

ApplicationResponse take(
    ApplicationRequestService& service,
    ApplicationRequester requester = ApplicationRequester::kUsb) {
  ApplicationResponse response;
  assert(service.takeResponse(requester, response));
  assert(response.requester == requester);
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
               ApplicationRequest{ApplicationRequester::kUsb, 7,
                                  ApplicationRequestKind::kGetConfig}) ==
           ApplicationSubmitResult::kAccepted);
    const ApplicationResponse response = take(service);
    assert(response.request_id == 7);
    assert(response.code == ApplicationResponseCode::kOk);
    assert(response.config_backend_ready);
    assert(!response.config_has_committed_record);
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
               ApplicationRequest{ApplicationRequester::kUsb, 42,
                                  ApplicationRequestKind::kGetConfig}) ==
           ApplicationSubmitResult::kAccepted);
    const ApplicationResponse response = take(service);
    assert(response.request_id == 42);
    assert(response.code == ApplicationResponseCode::kOk);
    assert(response.config_backend_ready);
    assert(response.config_has_committed_record);
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
               ApplicationRequest{ApplicationRequester::kUsb, 100,
                                  ApplicationRequestKind::kGetConfig}) ==
           ApplicationSubmitResult::kAccepted);
    assert(service.responsePending());
    assert(service.submit(
               ApplicationRequest{ApplicationRequester::kUsb, 101,
                                  ApplicationRequestKind::kGetConfig}) ==
           ApplicationSubmitResult::kBusy);

    const ApplicationResponse first = take(service);
    assert(first.request_id == 100);

    assert(service.submit(
               ApplicationRequest{ApplicationRequester::kUsb, 101,
                                  ApplicationRequestKind::kGetConfig}) ==
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
    assert(service.submit(ApplicationRequest{ApplicationRequester::kUsb, 200, unknown}) ==
           ApplicationSubmitResult::kAccepted);
    const ApplicationResponse response = take(service);
    assert(response.request_id == 200);
    assert(response.code == ApplicationResponseCode::kUnsupported);
    assert(!response.config_backend_ready);
    assert(!response.config_has_committed_record);
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
               ApplicationRequest{ApplicationRequester::kUsb, 300,
                                  ApplicationRequestKind::kGetConfig}) ==
           ApplicationSubmitResult::kAccepted);
    const ApplicationResponse response = take(service);
    assert(response.code == ApplicationResponseCode::kOk);
    assert(!response.config_backend_ready);
    assert(!response.config_has_committed_record);
    assert(response.config.tracking_interval_seconds == 180);
    assert(response.config.battery_capacity_mah == 0);
    assert(flash.program_calls == 0);
    assert(flash.erase_calls == 0);
  }

  // 6. A corrupt committed-looking page initializes the backend/store but is
  // not reported as a recovered durable record. This is distinct from case 5:
  // backend_ready=true, source remains default.
  {
    ReadOnlyFlash flash;
    flash.seedConfig(config_format::Config{600, 4000}, 1);
    flash.bytes[20] ^= 0xFF;  // break payload/CRC
    ConfigStore store(flash);
    assert(store.begin());
    assert(store.ready());
    assert(!store.hasCommittedRecord());
    assert(store.diagnostics().recovery_corruptions == 1);
    ApplicationRequestService service(store);

    assert(service.submit(
               ApplicationRequest{ApplicationRequester::kUsb, 400,
                                  ApplicationRequestKind::kGetConfig}) ==
           ApplicationSubmitResult::kAccepted);
    const ApplicationResponse response = take(service);
    assert(response.code == ApplicationResponseCode::kOk);
    assert(response.config_backend_ready);
    assert(!response.config_has_committed_record);
    assert(response.config.tracking_interval_seconds == 180);
    assert(response.config.battery_capacity_mah == 0);
    assert(flash.program_calls == 0);
    assert(flash.erase_calls == 0);
  }

  // 7. Requester ownership is enforced independently of numeric request IDs.
  // A USB consumer cannot steal or clear a BLE-owned response; the global
  // one-slot backpressure also remains in force until the rightful requester
  // consumes that result.
  {
    ReadOnlyFlash flash;
    ConfigStore store(flash);
    assert(store.begin());
    ApplicationRequestService service(store);

    assert(service.submit(ApplicationRequest{
               ApplicationRequester::kBle, 500,
               ApplicationRequestKind::kGetConfig}) ==
           ApplicationSubmitResult::kAccepted);

    ApplicationResponse wrong_consumer;
    assert(!service.takeResponse(ApplicationRequester::kUsb, wrong_consumer));
    assert(service.responsePending());

    assert(service.submit(ApplicationRequest{
               ApplicationRequester::kUsb, 500,
               ApplicationRequestKind::kGetConfig}) ==
           ApplicationSubmitResult::kBusy);

    const ApplicationResponse response =
        take(service, ApplicationRequester::kBle);
    assert(response.requester == ApplicationRequester::kBle);
    assert(response.request_id == 500);
    assert(response.code == ApplicationResponseCode::kOk);

    // The same numeric request_id is valid in another requester's namespace
    // once the prior response has been consumed.
    assert(service.submit(ApplicationRequest{
               ApplicationRequester::kUsb, 500,
               ApplicationRequestKind::kGetConfig}) ==
           ApplicationSubmitResult::kAccepted);
    const ApplicationResponse usb_response = take(service);
    assert(usb_response.requester == ApplicationRequester::kUsb);
    assert(usb_response.request_id == 500);
  }

  return 0;
}
