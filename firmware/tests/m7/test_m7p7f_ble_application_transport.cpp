// M7P7F: first frozen BLE application transport contract (pre-wire). Uses
// the real ApplicationRequestService + ConfigStore, exactly like
// test_m7p7d_app_request.cpp, so this proves BleApplicationTransport reads
// through the existing owners rather than a duplicated/second config path.
// No Bluefruit dependency anywhere in this file.
#include <assert.h>
#include <string.h>

#include <array>

#include "application_request.h"
#include "ble_application_transport.h"
#include "config_store.h"
#include "storage_config.h"

using namespace orun_tlp;
namespace bat = orun_tlp::ble_app_transport;

namespace {

constexpr size_t kRegionSize =
    storage_config::kPageSize * storage_config::kFutureConfigRegionPages;

class ReadOnlyFlash : public FlashBackend {
 public:
  std::array<uint8_t, kRegionSize> bytes{};
  unsigned program_calls = 0;
  unsigned erase_calls = 0;

  ReadOnlyFlash() { bytes.fill(0xFF); }

  bool begin() override { return true; }

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
                  uint64_t generation = 1, unsigned page = 0) {
    assert(page < storage_config::kFutureConfigRegionPages);
    uint8_t record[config_format::kRecordSize]{};
    config_format::encode(config, generation, record);
    memcpy(bytes.data() + page * storage_config::kPageSize, record,
           sizeof(record));
  }
};

void writeLE16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
uint16_t readLE16(const uint8_t* p) {
  return static_cast<uint16_t>(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}
uint32_t readLE32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

// Builds one raw wire frame into out (caller-owned, >=20 bytes). Returns the
// frame length (8 + payload_len).
uint8_t buildFrame(uint8_t* out, uint8_t version, uint8_t message_type,
                    uint8_t flags, uint8_t fragment_index,
                    uint16_t correlation_id, uint16_t total_length,
                    const uint8_t* payload, uint8_t payload_len) {
  out[0] = version;
  out[1] = message_type;
  out[2] = flags;
  out[3] = fragment_index;
  writeLE16(out + 4, correlation_id);
  writeLE16(out + 6, total_length);
  if (payload_len > 0) memcpy(out + 8, payload, payload_len);
  return static_cast<uint8_t>(8 + payload_len);
}

uint8_t buildGetConfigRequestFrame(uint8_t* out, uint16_t correlation_id) {
  return buildFrame(out, bat::kTransportVersion,
                     static_cast<uint8_t>(bat::MessageType::kGetConfigRequest),
                     bat::kFlagStart | bat::kFlagEnd, 0, correlation_id, 0,
                     nullptr, 0);
}

}  // namespace

int main() {
  // 1 & 2. Single-frame GET_CONFIG round trip with exact little-endian wire
  // bytes, against ConfigStore's blank-flash safe defaults.
  {
    ReadOnlyFlash flash;
    ConfigStore store(flash);
    assert(store.begin());
    ApplicationRequestService service(store);
    BleApplicationTransport transport(service);

    const uint32_t gen = transport.beginSession();
    (void)gen;

    uint8_t frame[bat::kMaxFrameSize];
    const uint8_t frame_len = buildGetConfigRequestFrame(frame, 0x1234);
    assert(frame_len == 8);
    transport.onFrameReceived(frame, frame_len, 1000);

    assert(transport.outboundFramePending());
    uint8_t out[bat::kMaxFrameSize];
    uint8_t out_len = 0;
    assert(transport.peekOutboundFrame(out, out_len));
    assert(out_len == 18);  // 8-byte header + 10-byte GET_CONFIG response.
    assert(out[0] == 0x01);  // transport version.
    assert(out[1] == 0x81);  // GET_CONFIG response.
    assert(out[2] == (bat::kFlagStart | bat::kFlagEnd));
    assert(out[3] == 0);  // single-frame response starts at fragment 0.
    assert(readLE16(out + 4) == 0x1234);  // echoed peer correlation id.
    assert(readLE16(out + 6) == 10);      // logical payload length.
    assert(out[8] == 0x00);               // application status OK.
    assert(out[9] == bat::kConfigFlagBackendReady);  // ready, no stored record.
    assert(readLE32(out + 10) == 180);    // default tracking_interval_seconds.
    assert(readLE32(out + 14) == 0);      // default battery_capacity_mah.
    assert(flash.program_calls == 0);
    assert(flash.erase_calls == 0);
  }

  // 3. Stored (non-default) config provenance bits, read through the real
  // ConfigStore owner rather than a duplicated application-side cache.
  {
    ReadOnlyFlash flash;
    flash.seedConfig(config_format::Config{247, 9000}, 3);
    ConfigStore store(flash);
    assert(store.begin());
    ApplicationRequestService service(store);
    BleApplicationTransport transport(service);
    transport.beginSession();

    uint8_t frame[bat::kMaxFrameSize];
    const uint8_t frame_len = buildGetConfigRequestFrame(frame, 7);
    transport.onFrameReceived(frame, frame_len, 0);

    uint8_t out[bat::kMaxFrameSize];
    uint8_t out_len = 0;
    assert(transport.peekOutboundFrame(out, out_len));
    assert(out_len == 18);
    assert(out[9] ==
           (bat::kConfigFlagBackendReady | bat::kConfigFlagHasCommittedRecord));
    assert(readLE32(out + 10) == 247);
    assert(readLE32(out + 14) == 9000);
  }

  // 4. A well-formed but unsupported message type produces ERROR/UNSUPPORTED
  // with the offending type echoed, and performs no application dispatch.
  {
    ReadOnlyFlash flash;
    ConfigStore store(flash);
    assert(store.begin());
    ApplicationRequestService service(store);
    BleApplicationTransport transport(service);
    transport.beginSession();

    uint8_t frame[bat::kMaxFrameSize];
    const uint8_t frame_len =
        buildFrame(frame, bat::kTransportVersion, 0x02,
                   bat::kFlagStart | bat::kFlagEnd, 0, 55, 0, nullptr, 0);
    transport.onFrameReceived(frame, frame_len, 0);

    assert(!service.responsePending());  // never reached the application seam.
    uint8_t out[bat::kMaxFrameSize];
    uint8_t out_len = 0;
    assert(transport.peekOutboundFrame(out, out_len));
    assert(out_len == 10);  // 8-byte header + 2-byte ERROR payload.
    assert(out[1] == 0xFF);
    assert(readLE16(out + 4) == 55);
    assert(readLE16(out + 6) == 2);
    assert(out[8] == static_cast<uint8_t>(bat::ErrorCode::kUnsupported));
    assert(out[9] == 0x02);  // offending message type.
  }

  // 5 & 6. Global ApplicationRequestService BUSY (held by USB) -> local
  // ERROR/BUSY. BLE can never consume or clear the USB-owned response, which
  // remains intact and USB-readable afterward.
  {
    ReadOnlyFlash flash;
    ConfigStore store(flash);
    assert(store.begin());
    ApplicationRequestService service(store);
    BleApplicationTransport transport(service);
    transport.beginSession();

    assert(service.submit(ApplicationRequest{ApplicationRequester::kUsb, 900,
                                              ApplicationRequestKind::kGetConfig}) ==
           ApplicationSubmitResult::kAccepted);

    ApplicationResponse stolen;
    assert(!service.takeResponse(ApplicationRequester::kBle, stolen));
    assert(!service.discardResponse(ApplicationRequester::kBle));
    assert(service.responsePending());

    uint8_t frame[bat::kMaxFrameSize];
    const uint8_t frame_len = buildGetConfigRequestFrame(frame, 321);
    transport.onFrameReceived(frame, frame_len, 0);

    uint8_t out[bat::kMaxFrameSize];
    uint8_t out_len = 0;
    assert(transport.peekOutboundFrame(out, out_len));
    assert(out_len == 10);
    assert(out[1] == 0xFF);
    assert(out[8] == static_cast<uint8_t>(bat::ErrorCode::kBusy));
    assert(out[9] == static_cast<uint8_t>(bat::MessageType::kGetConfigRequest));

    // USB's original response is untouched by any of the above.
    ApplicationResponse usb_response;
    assert(service.takeResponse(ApplicationRequester::kUsb, usb_response));
    assert(usb_response.request_id == 900);
  }

  // 7 & 8. BLE takes its response into the local outbound buffer promptly
  // (the global slot is free again immediately), so USB can subsequently use
  // the shared slot while the BLE response still awaits indication.
  {
    ReadOnlyFlash flash;
    ConfigStore store(flash);
    assert(store.begin());
    ApplicationRequestService service(store);
    BleApplicationTransport transport(service);
    transport.beginSession();

    uint8_t frame[bat::kMaxFrameSize];
    const uint8_t frame_len = buildGetConfigRequestFrame(frame, 1);
    transport.onFrameReceived(frame, frame_len, 0);

    assert(transport.outboundFramePending());
    assert(!service.responsePending());  // released immediately.

    assert(service.submit(ApplicationRequest{ApplicationRequester::kUsb, 2,
                                              ApplicationRequestKind::kGetConfig}) ==
           ApplicationSubmitResult::kAccepted);
    ApplicationResponse usb_response;
    assert(service.takeResponse(ApplicationRequester::kUsb, usb_response));
    assert(usb_response.request_id == 2);

    // The BLE-owned local outbound response is unaffected by USB's traffic.
    assert(transport.outboundFramePending());
  }

  // 9. A non-reading BLE client cannot overwrite its own pending outbound
  // response with a second request (stop-and-wait backpressure).
  {
    ReadOnlyFlash flash;
    ConfigStore store(flash);
    assert(store.begin());
    ApplicationRequestService service(store);
    BleApplicationTransport transport(service);
    transport.beginSession();

    uint8_t frame_a[bat::kMaxFrameSize];
    transport.onFrameReceived(frame_a, buildGetConfigRequestFrame(frame_a, 10),
                               0);
    assert(transport.outboundFramePending());

    uint8_t frame_b[bat::kMaxFrameSize];
    transport.onFrameReceived(frame_b, buildGetConfigRequestFrame(frame_b, 20),
                               100);

    // Still the first (unread) response; the global slot was never touched
    // by the rejected second request.
    assert(!service.responsePending());
    uint8_t out[bat::kMaxFrameSize];
    uint8_t out_len = 0;
    assert(transport.peekOutboundFrame(out, out_len));
    assert(readLE16(out + 4) == 10);
  }

  // 10 & 11. Indication retry returns the identical frame until confirmed;
  // the response is removed only after the final (here, only) fragment is
  // confirmed.
  {
    ReadOnlyFlash flash;
    ConfigStore store(flash);
    assert(store.begin());
    ApplicationRequestService service(store);
    BleApplicationTransport transport(service);
    transport.beginSession();

    uint8_t frame[bat::kMaxFrameSize];
    transport.onFrameReceived(frame, buildGetConfigRequestFrame(frame, 1), 0);

    uint8_t first[bat::kMaxFrameSize];
    uint8_t first_len = 0;
    assert(transport.peekOutboundFrame(first, first_len));
    uint8_t second[bat::kMaxFrameSize];
    uint8_t second_len = 0;
    assert(transport.peekOutboundFrame(second, second_len));
    assert(first_len == second_len);
    assert(memcmp(first, second, first_len) == 0);
    assert(transport.outboundFramePending());  // still pending, unconfirmed.

    transport.confirmOutboundFrame();
    assert(!transport.outboundFramePending());
    uint8_t after[bat::kMaxFrameSize];
    uint8_t after_len = 0;
    assert(!transport.peekOutboundFrame(after, after_len));
  }

  // 12 & 13. Disconnect clears the outbound response; a subsequent
  // reconnect starts with nothing pending (cannot receive prior-session
  // data).
  {
    ReadOnlyFlash flash;
    ConfigStore store(flash);
    assert(store.begin());
    ApplicationRequestService service(store);
    BleApplicationTransport transport(service);
    const uint32_t gen1 = transport.beginSession();

    uint8_t frame[bat::kMaxFrameSize];
    transport.onFrameReceived(frame, buildGetConfigRequestFrame(frame, 1), 0);
    assert(transport.outboundFramePending());

    transport.endSession(gen1);
    assert(!transport.outboundFramePending());
    assert(!service.responsePending());

    transport.beginSession();
    assert(!transport.outboundFramePending());
    uint8_t out[bat::kMaxFrameSize];
    uint8_t out_len = 0;
    assert(!transport.peekOutboundFrame(out, out_len));
  }

  // 14. A delayed disconnect for an already-replaced session must never
  // clear the replacement session's response.
  {
    ReadOnlyFlash flash;
    ConfigStore store(flash);
    assert(store.begin());
    ApplicationRequestService service(store);
    BleApplicationTransport transport(service);

    const uint32_t gen1 = transport.beginSession();
    uint8_t old_frame[bat::kMaxFrameSize];
    transport.onFrameReceived(old_frame,
                               buildGetConfigRequestFrame(old_frame, 111), 0);
    assert(transport.outboundFramePending());

    // A new connection is accepted before the old session's disconnect
    // cleanup has arrived (defensively clears the stale session 1 state).
    const uint32_t gen2 = transport.beginSession();
    assert(gen2 != gen1);
    assert(!transport.outboundFramePending());

    uint8_t new_frame[bat::kMaxFrameSize];
    transport.onFrameReceived(new_frame,
                               buildGetConfigRequestFrame(new_frame, 222), 50);
    assert(transport.outboundFramePending());

    // The stale, delayed disconnect for session 1 finally arrives.
    transport.endSession(gen1);

    // Session 2's response must be untouched.
    assert(transport.outboundFramePending());
    uint8_t out[bat::kMaxFrameSize];
    uint8_t out_len = 0;
    assert(transport.peekOutboundFrame(out, out_len));
    assert(readLE16(out + 4) == 222);
  }

  // 15 & 16. The same peer uint16 correlation id repeating in a different
  // session is safe: local session/internal request ownership is
  // independent of peer-controlled bytes, and the local uint32 request id
  // keeps advancing rather than resetting to 1 on reconnect.
  {
    ReadOnlyFlash flash;
    ConfigStore store(flash);
    assert(store.begin());
    ApplicationRequestService service(store);
    BleApplicationTransport transport(service);

    transport.beginSession();
    uint8_t frame1[bat::kMaxFrameSize];
    transport.onFrameReceived(frame1, buildGetConfigRequestFrame(frame1, 999),
                               0);
    const uint32_t id_after_session1 = transport.debugNextLocalRequestId();
    assert(id_after_session1 > 1);
    transport.confirmOutboundFrame();

    transport.beginSession();  // reconnect
    uint8_t frame2[bat::kMaxFrameSize];
    transport.onFrameReceived(frame2, buildGetConfigRequestFrame(frame2, 999),
                               0);
    assert(transport.debugNextLocalRequestId() == id_after_session1 + 1);

    uint8_t out[bat::kMaxFrameSize];
    uint8_t out_len = 0;
    assert(transport.peekOutboundFrame(out, out_len));
    assert(readLE16(out + 4) == 999);  // peer correlation id echoed correctly.
  }

  // 17. Request-id wraparound skips zero (tested as a pure function so the
  // test does not need to submit 2^32 requests).
  {
    assert(bat::advanceRequestId(0xFFFFFFFFUL) == 1);
    assert(bat::advanceRequestId(5) == 6);
    assert(bat::advanceRequestId(0xFFFFFFFEUL) == 0xFFFFFFFFUL);
  }

  // 18. A disconnect while a fragment is only partially reassembled leaves
  // no lingering state (also covered generally by malformed-input case r
  // below).
  {
    ReadOnlyFlash flash;
    ConfigStore store(flash);
    assert(store.begin());
    ApplicationRequestService service(store);
    BleApplicationTransport transport(service);
    const uint32_t gen = transport.beginSession();

    uint8_t chunk[12];
    memset(chunk, 0xAA, sizeof(chunk));
    uint8_t frame[bat::kMaxFrameSize];
    const uint8_t frame_len =
        buildFrame(frame, bat::kTransportVersion, 0x02, bat::kFlagStart, 0, 1,
                   24, chunk, sizeof(chunk));
    transport.onFrameReceived(frame, frame_len, 0);
    assert(transport.inboundReassemblyActive());

    transport.endSession(gen);
    assert(!transport.inboundReassemblyActive());
    assert(!transport.outboundFramePending());
  }

  // 19. Malformed-input matrix (docs/milestones/M7P7F.md section 12). Every
  // case must fail with no application/storage side effect and, where noted,
  // must clear any in-progress partial reassembly. A trailing flood of
  // garbage followed by one legitimate request proves the state machine
  // never wedges and never grows unbounded state.
  {
    ReadOnlyFlash flash;
    ConfigStore store(flash);
    assert(store.begin());
    ApplicationRequestService service(store);
    BleApplicationTransport transport(service);
    transport.beginSession();

    uint8_t chunk12[12];
    memset(chunk12, 0x11, sizeof(chunk12));

    // a. frame shorter than 8 bytes.
    {
      uint8_t frame[7]{1, 1, 3, 0, 0, 0, 0};
      transport.onFrameReceived(frame, sizeof(frame), 0);
      assert(!transport.inboundReassemblyActive());
    }
    // b. frame longer than 20 bytes.
    {
      uint8_t frame[21]{};
      frame[0] = bat::kTransportVersion;
      frame[2] = bat::kFlagStart | bat::kFlagEnd;
      transport.onFrameReceived(frame, sizeof(frame), 0);
      assert(!transport.inboundReassemblyActive());
      assert(!transport.outboundFramePending());
    }
    // c. wrong version.
    {
      uint8_t frame[bat::kMaxFrameSize];
      const uint8_t len =
          buildFrame(frame, 0x02, 0x01, bat::kFlagStart | bat::kFlagEnd, 0, 1,
                     0, nullptr, 0);
      transport.onFrameReceived(frame, len, 0);
      assert(!transport.outboundFramePending());
    }
    // d. unknown flag bits.
    {
      uint8_t frame[bat::kMaxFrameSize];
      const uint8_t len = buildFrame(frame, bat::kTransportVersion, 0x01,
                                      0x04, 0, 1, 0, nullptr, 0);
      transport.onFrameReceived(frame, len, 0);
      assert(!transport.inboundReassemblyActive());
    }
    // e. logical length > 48.
    {
      uint8_t frame[bat::kMaxFrameSize];
      const uint8_t len = buildFrame(frame, bat::kTransportVersion, 0x02,
                                      bat::kFlagStart, 0, 1, 49, chunk12,
                                      sizeof(chunk12));
      transport.onFrameReceived(frame, len, 0);
      assert(!transport.inboundReassemblyActive());
    }
    // f. fragment payload exceeding declared total.
    {
      uint8_t frame[bat::kMaxFrameSize];
      const uint8_t len = buildFrame(frame, bat::kTransportVersion, 0x02,
                                      bat::kFlagStart, 0, 1, 5, chunk12,
                                      sizeof(chunk12));
      transport.onFrameReceived(frame, len, 0);
      assert(!transport.inboundReassemblyActive());
    }
    // g. missing START (bare continuation).
    {
      uint8_t frame[bat::kMaxFrameSize];
      const uint8_t len = buildFrame(frame, bat::kTransportVersion, 0x02, 0,
                                      1, 1, 24, chunk12, sizeof(chunk12));
      transport.onFrameReceived(frame, len, 0);
      assert(!transport.inboundReassemblyActive());
    }
    // h. START with non-zero fragment index.
    {
      uint8_t frame[bat::kMaxFrameSize];
      const uint8_t len =
          buildFrame(frame, bat::kTransportVersion, 0x02, bat::kFlagStart, 1,
                     1, 24, chunk12, sizeof(chunk12));
      transport.onFrameReceived(frame, len, 0);
      assert(!transport.inboundReassemblyActive());
    }
    // i. unexpected new START during a partial message clears the old one
    // and processes the new one on its own terms.
    {
      uint8_t partial[bat::kMaxFrameSize];
      transport.onFrameReceived(
          partial,
          buildFrame(partial, bat::kTransportVersion, 0x02, bat::kFlagStart, 0,
                     1, 24, chunk12, sizeof(chunk12)),
          0);
      assert(transport.inboundReassemblyActive());

      uint8_t fresh[bat::kMaxFrameSize];
      transport.onFrameReceived(fresh, buildGetConfigRequestFrame(fresh, 42),
                                 10);
      assert(!transport.inboundReassemblyActive());
      assert(transport.outboundFramePending());
      uint8_t out[bat::kMaxFrameSize];
      uint8_t out_len = 0;
      assert(transport.peekOutboundFrame(out, out_len));
      assert(readLE16(out + 4) == 42);  // the NEW request's correlation id.
      transport.confirmOutboundFrame();
    }
    // j. duplicate fragment.
    {
      uint8_t start[bat::kMaxFrameSize];
      transport.onFrameReceived(
          start,
          buildFrame(start, bat::kTransportVersion, 0x02, bat::kFlagStart, 0,
                     2, 24, chunk12, sizeof(chunk12)),
          0);
      assert(transport.inboundReassemblyActive());
      uint8_t dup[bat::kMaxFrameSize];
      transport.onFrameReceived(
          dup, buildFrame(dup, bat::kTransportVersion, 0x02, 0, 0, 2, 24,
                           chunk12, sizeof(chunk12)),
          0);
      assert(!transport.inboundReassemblyActive());
    }
    // k. skipped fragment (needs 3 fragments for total_length=36).
    {
      uint8_t start[bat::kMaxFrameSize];
      transport.onFrameReceived(
          start,
          buildFrame(start, bat::kTransportVersion, 0x02, bat::kFlagStart, 0,
                     3, 36, chunk12, sizeof(chunk12)),
          0);
      assert(transport.inboundReassemblyActive());
      uint8_t skip[bat::kMaxFrameSize];
      transport.onFrameReceived(
          skip, buildFrame(skip, bat::kTransportVersion, 0x02, 0, 2, 3, 36,
                            chunk12, sizeof(chunk12)),
          0);
      assert(!transport.inboundReassemblyActive());
    }
    // l. correlation-id change mid-message.
    {
      uint8_t start[bat::kMaxFrameSize];
      transport.onFrameReceived(
          start,
          buildFrame(start, bat::kTransportVersion, 0x02, bat::kFlagStart, 0,
                     4, 24, chunk12, sizeof(chunk12)),
          0);
      uint8_t cont[bat::kMaxFrameSize];
      transport.onFrameReceived(
          cont, buildFrame(cont, bat::kTransportVersion, 0x02, 0, 1, 5, 24,
                            chunk12, sizeof(chunk12)),
          0);
      assert(!transport.inboundReassemblyActive());
    }
    // m. message-type change mid-message.
    {
      uint8_t start[bat::kMaxFrameSize];
      transport.onFrameReceived(
          start,
          buildFrame(start, bat::kTransportVersion, 0x02, bat::kFlagStart, 0,
                     6, 24, chunk12, sizeof(chunk12)),
          0);
      uint8_t cont[bat::kMaxFrameSize];
      transport.onFrameReceived(
          cont, buildFrame(cont, bat::kTransportVersion, 0x03, 0, 1, 6, 24,
                            chunk12, sizeof(chunk12)),
          0);
      assert(!transport.inboundReassemblyActive());
    }
    // n. total-length change mid-message.
    {
      uint8_t start[bat::kMaxFrameSize];
      transport.onFrameReceived(
          start,
          buildFrame(start, bat::kTransportVersion, 0x02, bat::kFlagStart, 0,
                     7, 24, chunk12, sizeof(chunk12)),
          0);
      uint8_t cont[bat::kMaxFrameSize];
      transport.onFrameReceived(
          cont, buildFrame(cont, bat::kTransportVersion, 0x02, 0, 1, 7, 30,
                            chunk12, sizeof(chunk12)),
          0);
      assert(!transport.inboundReassemblyActive());
    }
    // o. END too early / END with wrong final length (received length would
    // be 18, not the declared 24).
    {
      uint8_t start[bat::kMaxFrameSize];
      transport.onFrameReceived(
          start,
          buildFrame(start, bat::kTransportVersion, 0x02, bat::kFlagStart, 0,
                     8, 24, chunk12, sizeof(chunk12)),
          0);
      uint8_t six[6];
      memset(six, 0x22, sizeof(six));
      uint8_t cont[bat::kMaxFrameSize];
      transport.onFrameReceived(
          cont, buildFrame(cont, bat::kTransportVersion, 0x02, bat::kFlagEnd,
                            1, 8, 24, six, sizeof(six)),
          0);
      assert(!transport.inboundReassemblyActive());
      assert(!transport.outboundFramePending());
    }
    // p & q. exact declared length reached without END stays pending, then
    // fails closed only via the bounded fragment timeout (no background
    // timer -- loop-owned poll()).
    {
      uint8_t start[bat::kMaxFrameSize];
      transport.onFrameReceived(
          start,
          buildFrame(start, bat::kTransportVersion, 0x02, bat::kFlagStart, 0,
                     9, 12, chunk12, sizeof(chunk12)),
          5000);
      assert(transport.inboundReassemblyActive());  // exact length, no END.
      transport.poll(5000 + bat::kFragmentTimeoutMs - 1);
      assert(transport.inboundReassemblyActive());  // not yet timed out.
      transport.poll(5000 + bat::kFragmentTimeoutMs);
      assert(!transport.inboundReassemblyActive());  // timed out, discarded.
      assert(!transport.outboundFramePending());      // no application effect.
    }
    // Fragment-count bound: 5 small fragments for a 20-byte logical length
    // must be rejected once the 4-fragment ceiling is exceeded, independent
    // of the length-derived ceiling.
    {
      uint8_t piece[4];
      memset(piece, 0x33, sizeof(piece));
      uint8_t start[bat::kMaxFrameSize];
      transport.onFrameReceived(
          start, buildFrame(start, bat::kTransportVersion, 0x02,
                             bat::kFlagStart, 0, 10, 20, piece, sizeof(piece)),
          0);
      for (uint8_t idx = 1; idx <= 3; ++idx) {
        uint8_t cont[bat::kMaxFrameSize];
        transport.onFrameReceived(
            cont, buildFrame(cont, bat::kTransportVersion, 0x02, 0, idx, 10,
                              20, piece, sizeof(piece)),
            0);
        assert(transport.inboundReassemblyActive());
      }
      uint8_t fifth[bat::kMaxFrameSize];
      transport.onFrameReceived(
          fifth, buildFrame(fifth, bat::kTransportVersion, 0x02, 0, 4, 10, 20,
                             piece, sizeof(piece)),
          0);
      assert(!transport.inboundReassemblyActive());
    }
    // GET_CONFIG with a nonzero declared length violates its frozen
    // zero-length contract: fails closed, no reply, no dispatch.
    {
      uint8_t one[1] = {0x00};
      uint8_t frame[bat::kMaxFrameSize];
      const uint8_t len = buildFrame(
          frame, bat::kTransportVersion,
          static_cast<uint8_t>(bat::MessageType::kGetConfigRequest),
          bat::kFlagStart | bat::kFlagEnd, 0, 11, 1, one, sizeof(one));
      transport.onFrameReceived(frame, len, 0);
      assert(!service.responsePending());
      assert(!transport.outboundFramePending());
    }

    // r. Flood of garbage frames followed by one legitimate request: state
    // machine never wedges, no unbounded growth (all state is fixed-size
    // members), and the legitimate request still succeeds normally.
    for (int i = 0; i < 200; ++i) {
      uint8_t garbage[bat::kMaxFrameSize];
      memset(garbage, static_cast<uint8_t>(i), sizeof(garbage));
      garbage[0] = 0x09;  // always a wrong version.
      transport.onFrameReceived(garbage, bat::kMaxFrameSize, 0);
    }
    assert(!transport.inboundReassemblyActive());
    assert(!transport.outboundFramePending());

    uint8_t final_frame[bat::kMaxFrameSize];
    transport.onFrameReceived(
        final_frame, buildGetConfigRequestFrame(final_frame, 4242), 0);
    assert(transport.outboundFramePending());
    uint8_t out[bat::kMaxFrameSize];
    uint8_t out_len = 0;
    assert(transport.peekOutboundFrame(out, out_len));
    assert(out[1] == 0x81);
    assert(readLE16(out + 4) == 4242);

    assert(flash.program_calls == 0);
    assert(flash.erase_calls == 0);
  }

  return 0;
}
