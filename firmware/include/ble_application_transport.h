#pragma once

#include <stdint.h>

#include "application_request.h"

namespace orun_tlp {

// M7P7F: first frozen BLE application transport contract. This header/its
// .cpp have NO Bluefruit/Arduino dependency -- it is driven entirely by
// loop/task-owned code (future M7P7G) and is fully host-testable today.
//
// Scope: transport framing, fragmentation/reassembly, GET_CONFIG dispatch
// through the existing ApplicationRequestService, stop-and-wait backpressure
// and bounded session/generation hygiene. It does NOT instantiate a
// Bluefruit BLEService/BLECharacteristic, does not change BLE admission, and
// exposes no operation beyond the pre-authorization read-only GET_CONFIG
// allowlist documented in docs/milestones/M7P7F.md section 6. See that
// document for the full contract and rationale.
namespace ble_app_transport {

// ---------------------------------------------------------------------------
// Frozen 128-bit UUIDs (generated once; never derived from DeviceIdentity or
// any runtime value). Stored here in the plain 16-byte RFC 4122 big-endian
// order in which they were generated (matches the printed canonical string
// below byte-for-byte). M7P7F does not instantiate any BLEUuid/BLEService/
// BLECharacteristic object; a future M7P7G adapter is responsible for
// converting to whatever byte order Bluefruit's BLEUuid constructor expects
// (typically the reverse, little-endian, on-air order) -- the identity of
// the 128-bit value is what is frozen here, not a particular in-memory byte
// order.
//
// ORUN application service:      0088f2c3-cc13-4b5d-b025-f7fcb7c71af2
// ORUN application request char: fbf521f6-ce25-4e6a-af6c-c76303803ee3
// ORUN application response char:c0ba4044-1242-447b-9f79-e97aa0065ad3
constexpr uint8_t kServiceUuid128[16] = {
    0x00, 0x88, 0xF2, 0xC3, 0xCC, 0x13, 0x4B, 0x5D,
    0xB0, 0x25, 0xF7, 0xFC, 0xB7, 0xC7, 0x1A, 0xF2};
constexpr uint8_t kRequestCharacteristicUuid128[16] = {
    0xFB, 0xF5, 0x21, 0xF6, 0xCE, 0x25, 0x4E, 0x6A,
    0xAF, 0x6C, 0xC7, 0x63, 0x03, 0x80, 0x3E, 0xE3};
constexpr uint8_t kResponseCharacteristicUuid128[16] = {
    0xC0, 0xBA, 0x40, 0x44, 0x12, 0x42, 0x44, 0x7B,
    0x9F, 0x79, 0xE9, 0x7A, 0xA0, 0x06, 0x5A, 0xD3};

// ---------------------------------------------------------------------------
// Wire frame contract (docs/milestones/M7P7F.md section 4). Fixed at ATT MTU
// 23; no MTU negotiation is required or assumed.
constexpr uint8_t kTransportVersion = 0x01;
constexpr uint8_t kHeaderSize = 8;
constexpr uint8_t kMaxFrameSize = 20;
constexpr uint8_t kMaxPayloadPerFrame = kMaxFrameSize - kHeaderSize;  // 12
constexpr uint16_t kMaxLogicalPayload = 48;
constexpr uint8_t kMaxFragments = 4;
constexpr uint32_t kFragmentTimeoutMs = 2000;

constexpr uint8_t kFlagStart = 0x01;
constexpr uint8_t kFlagEnd = 0x02;
constexpr uint8_t kValidFlagsMask = kFlagStart | kFlagEnd;

enum class MessageType : uint8_t {
  kGetConfigRequest = 0x01,
  kGetConfigResponse = 0x81,
  kError = 0xFF,
};

// GET_CONFIG response flags byte (docs/milestones/M7P7F.md section 5).
constexpr uint8_t kConfigFlagBackendReady = 0x01;
constexpr uint8_t kConfigFlagHasCommittedRecord = 0x02;

constexpr uint8_t kApplicationStatusOk = 0x00;

enum class ErrorCode : uint8_t {
  kUnsupported = 0x01,
  kBusy = 0x02,
};

// Advances a local monotonic request-id counter, skipping zero on
// wraparound (docs/milestones/M7P7F.md section 7). Exposed as a pure free
// function, mirroring monotonic_time.h's testable-helper style, so host
// tests can verify the skip-zero wrap rule directly instead of needing to
// actually submit 2^32 requests.
constexpr uint32_t advanceRequestId(uint32_t current) {
  return (current + 1) == 0 ? 1 : (current + 1);
}

}  // namespace ble_app_transport

// Bounded, fixed-memory BLE application session/transport state. Exactly one
// active session, one partial inbound logical message and one outbound
// logical response at a time -- matching this milestone's single-BLE-client
// product scope. No heap allocation, no std::vector/std::string.
//
// ApplicationRequester::kBle is always the value this class submits; it is
// never derived from any peer-controlled byte (frame header, correlation id,
// message type or payload). See docs/architecture/ORUN_CURRENT_ARCHITECTURE_RULES.md
// section 17 and docs/milestones/M7P7E.md for why that boundary matters.
class BleApplicationTransport {
 public:
  explicit BleApplicationTransport(ApplicationRequestService& service)
      : service_(service) {}

  // ---- Session lifecycle (future M7P7G calls these around connect/disconnect) ----

  // Call once per newly accepted BLE application connection/session. Clears
  // any leftover inbound/outbound state (defensively; normal disconnect
  // cleanup should already have done so) and starts a new local session
  // generation. Returns that generation so the caller can hand it back to
  // endSession() later -- a delayed/duplicate disconnect for an old,
  // already-replaced session can then never affect the new one. The
  // generation is local-only bookkeeping; it is never sent on the wire.
  uint32_t beginSession();

  // Call once per observed BLE disconnect, passing the generation captured
  // from beginSession() at connect time. A stale generation (the session was
  // already replaced by a newer beginSession()) makes this a safe no-op.
  // Clears partial inbound reassembly, clears any unsent outbound response,
  // and discards a BLE-owned ApplicationRequestService response if one is
  // still pending -- never a USB-owned or any other requester's response.
  void endSession(uint32_t session_generation);

  uint32_t currentSessionGeneration() const { return session_generation_; }

  // ---- Inbound ----

  // Feed one raw transport frame (already MTU/length-bounded by the future
  // caller's read, but validated fully here regardless). now: loop-owned
  // monotonic ms, used only for fragment-timeout bookkeeping. Malformed,
  // out-of-order, duplicate or otherwise invalid frames fail closed: no
  // application/storage/radio side effect occurs, and any in-progress
  // partial reassembly is conservatively cleared. A well-formed new START
  // always invalidates/clears a different in-progress partial message
  // before starting the new one, per docs/milestones/M7P7F.md section 4.
  void onFrameReceived(const uint8_t* frame, uint8_t frame_len, uint32_t now);

  // Loop-owned: call every tick to advance the bounded fragment-reassembly
  // timeout. No background timer/thread is used.
  void poll(uint32_t now);

  bool inboundReassemblyActive() const { return inbound_.active; }

  // Test-only introspection of the next local application request id that
  // will be assigned. This is local adapter bookkeeping only: it is never
  // sent on the wire and carries no authorization meaning.
  uint32_t debugNextLocalRequestId() const { return next_local_request_id_; }

  // ---- Outbound (future M7P7G indication adapter) ----

  bool outboundFramePending() const { return outbound_.pending; }

  // Copies the next <=20-byte frame to send into frame_out (caller-owned,
  // >= kMaxFrameSize bytes) and reports its length. Pure peek: repeatable,
  // does not advance state, so a failed/unconfirmed indication can be
  // retried with the identical frame. Returns false if no outbound frame is
  // pending.
  bool peekOutboundFrame(uint8_t* frame_out, uint8_t& frame_len) const;

  // Caller observed successful indication/confirmation of the frame last
  // returned by peekOutboundFrame(). Advances the outbound fragment cursor;
  // clears the whole logical response only once its final fragment has been
  // confirmed. A no-op if no outbound frame is pending.
  void confirmOutboundFrame();

 private:
  struct InboundReassembly {
    bool active = false;
    uint8_t version = 0;
    uint8_t message_type = 0;
    uint16_t correlation_id = 0;
    uint16_t total_length = 0;
    uint16_t received_length = 0;
    uint8_t next_fragment_index = 0;
    uint32_t last_fragment_at_ms = 0;
    uint8_t payload[ble_app_transport::kMaxLogicalPayload]{};
  };

  struct OutboundResponse {
    bool pending = false;
    uint8_t message_type = 0;
    uint16_t correlation_id = 0;
    uint16_t total_length = 0;
    uint8_t next_fragment_index = 0;
    uint8_t payload[ble_app_transport::kMaxLogicalPayload]{};
  };

  void clearInbound();
  void beginInbound(uint8_t version, uint8_t message_type,
                     uint16_t correlation_id, uint16_t total_length,
                     uint32_t now);
  void dispatchInbound();
  void buildGetConfigResponse(uint16_t correlation_id,
                               const ApplicationResponse& response);
  void buildErrorResponse(uint16_t correlation_id,
                           ble_app_transport::ErrorCode code,
                           uint8_t offending_message_type);
  uint32_t nextLocalRequestId();

  ApplicationRequestService& service_;
  InboundReassembly inbound_{};
  OutboundResponse outbound_{};
  // Never reset by beginSession()/endSession(): must not restart at 1 on
  // every reconnect (docs/milestones/M7P7F.md section 7). Zero is reserved
  // and skipped on wraparound.
  uint32_t next_local_request_id_ = 1;
  uint32_t session_generation_ = 0;
};

}  // namespace orun_tlp
