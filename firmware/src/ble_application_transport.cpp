#include "ble_application_transport.h"

#include <string.h>

#include "monotonic_time.h"

namespace orun_tlp {

namespace {

using ble_app_transport::kFlagEnd;
using ble_app_transport::kFlagStart;
using ble_app_transport::kFragmentTimeoutMs;
using ble_app_transport::kHeaderSize;
using ble_app_transport::kMaxFragments;
using ble_app_transport::kMaxFrameSize;
using ble_app_transport::kMaxLogicalPayload;
using ble_app_transport::kMaxPayloadPerFrame;
using ble_app_transport::kTransportVersion;
using ble_app_transport::kValidFlagsMask;
using ble_app_transport::MessageType;

// Wire encoding is little-endian per docs/milestones/M7P7F.md section 4.
// journal_format::put16/put32 are big-endian, so this transport keeps its
// own small helpers rather than reusing them.
void writeLE16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void writeLE32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}
uint16_t readLE16(const uint8_t* p) {
  return static_cast<uint16_t>(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}

}  // namespace

uint32_t BleApplicationTransport::beginSession() {
  clearInbound();
  outbound_ = OutboundResponse();
  // Defensive: a BLE-owned global response should already be gone (this
  // adapter always takes it promptly on submit), but never leave the global
  // slot wedged for a fresh session. discardResponse() is a no-op unless the
  // pending response actually belongs to kBle, so a USB-owned response is
  // never touched.
  service_.discardResponse(ApplicationRequester::kBle);
  session_generation_ =
      ble_app_transport::advanceSessionGeneration(session_generation_);
  session_active_ = true;
  return session_generation_;
}

void BleApplicationTransport::endSession(uint32_t session_generation) {
  // A stale/delayed disconnect for a session that has already been replaced
  // by a newer beginSession() must never clear the replacement session.
  if (!session_active_ || session_generation != session_generation_) return;
  clearInbound();
  outbound_ = OutboundResponse();
  service_.discardResponse(ApplicationRequester::kBle);
  session_active_ = false;
}

void BleApplicationTransport::clearInbound() { inbound_ = InboundReassembly(); }

void BleApplicationTransport::beginInbound(uint8_t version,
                                            uint8_t message_type,
                                            uint16_t correlation_id,
                                            uint16_t total_length,
                                            uint32_t now) {
  inbound_ = InboundReassembly();
  inbound_.active = true;
  inbound_.version = version;
  inbound_.message_type = message_type;
  inbound_.correlation_id = correlation_id;
  inbound_.total_length = total_length;
  inbound_.last_fragment_at_ms = now;
}

void BleApplicationTransport::onFrameReceived(uint32_t session_generation,
                                               const uint8_t* frame,
                                               uint8_t frame_len,
                                               uint32_t now) {
  // Event provenance is checked before touching any current-session state.
  // A queued frame from a disconnected/replaced session must be a pure no-op.
  if (!session_active_ || session_generation != session_generation_) return;

  // Stop-and-wait begins at ingress, not only after reassembly. While a prior
  // response awaits indication confirmation, the peer cannot pre-stage a
  // partial next request and complete it immediately after confirmation.
  if (outbound_.pending) {
    clearInbound();
    return;
  }

  // Any validation failure below fails closed: no application/storage side
  // effect, and any in-progress partial reassembly is conservatively
  // cleared rather than left in a possibly-inconsistent state.
  if (frame == nullptr || frame_len < kHeaderSize ||
      frame_len > kMaxFrameSize) {
    clearInbound();
    return;
  }

  const uint8_t version = frame[0];
  const uint8_t message_type = frame[1];
  const uint8_t flags = frame[2];
  const uint8_t fragment_index = frame[3];
  const uint16_t correlation_id = readLE16(frame + 4);
  const uint16_t total_length = readLE16(frame + 6);
  const uint8_t fragment_payload_len =
      static_cast<uint8_t>(frame_len - kHeaderSize);

  if (version != kTransportVersion) {
    clearInbound();
    return;
  }
  if ((flags & static_cast<uint8_t>(~kValidFlagsMask)) != 0) {
    clearInbound();
    return;
  }
  if (total_length > kMaxLogicalPayload) {
    clearInbound();
    return;
  }

  const bool is_start = (flags & kFlagStart) != 0;
  const bool is_end = (flags & kFlagEnd) != 0;

  if (is_start) {
    if (fragment_index != 0) {
      // START with non-zero fragment index is malformed.
      clearInbound();
      return;
    }
    // A new START always invalidates/clears a different in-progress partial
    // message before processing this one.
    beginInbound(version, message_type, correlation_id, total_length, now);
    if (fragment_payload_len > kMaxPayloadPerFrame ||
        fragment_payload_len > total_length) {
      clearInbound();
      return;
    }
    memcpy(inbound_.payload, frame + kHeaderSize, fragment_payload_len);
    inbound_.received_length = fragment_payload_len;
    inbound_.next_fragment_index = 1;
    inbound_.last_fragment_at_ms = now;
    if (is_end) {
      if (inbound_.received_length != inbound_.total_length) {
        clearInbound();
        return;
      }
      dispatchInbound();
    }
    return;
  }

  // Continuation fragment: must match an active reassembly exactly.
  if (!inbound_.active) {
    // Missing START.
    clearInbound();
    return;
  }
  if (version != inbound_.version || message_type != inbound_.message_type ||
      correlation_id != inbound_.correlation_id ||
      total_length != inbound_.total_length) {
    // correlation-id / message-type / total-length change mid-message.
    clearInbound();
    return;
  }
  if (fragment_index != inbound_.next_fragment_index) {
    // Duplicate or skipped/out-of-order fragment.
    clearInbound();
    return;
  }
  if (inbound_.next_fragment_index >= kMaxFragments) {
    // Would exceed the 4-fragment bound.
    clearInbound();
    return;
  }
  if (fragment_payload_len > kMaxPayloadPerFrame ||
      uint16_t(inbound_.received_length + fragment_payload_len) >
          inbound_.total_length) {
    // Fragment payload exceeds the declared total (also covers "END too
    // early" style overshoot).
    clearInbound();
    return;
  }

  memcpy(inbound_.payload + inbound_.received_length, frame + kHeaderSize,
         fragment_payload_len);
  inbound_.received_length =
      static_cast<uint16_t>(inbound_.received_length + fragment_payload_len);
  inbound_.next_fragment_index =
      static_cast<uint8_t>(inbound_.next_fragment_index + 1);
  inbound_.last_fragment_at_ms = now;

  if (is_end) {
    if (inbound_.received_length != inbound_.total_length) {
      // END with wrong final length.
      clearInbound();
      return;
    }
    dispatchInbound();
  }
  // Exact declared length reached without END: stays pending until either a
  // (rejected) further fragment, an END frame, or the fragment timeout.
}

void BleApplicationTransport::poll(uint32_t now) {
  if (!inbound_.active) return;
  if (monotonic::elapsed(now, inbound_.last_fragment_at_ms,
                          kFragmentTimeoutMs)) {
    clearInbound();
  }
}

void BleApplicationTransport::dispatchInbound() {
  const uint16_t correlation_id = inbound_.correlation_id;
  const uint8_t message_type = inbound_.message_type;
  const uint16_t total_length = inbound_.total_length;
  // Snapshot what is needed, then clear reassembly before dispatch so any
  // early return below never leaves partial inbound state lingering.
  clearInbound();

  if (outbound_.pending) {
    // Stop-and-wait backpressure (docs/milestones/M7P7F.md section 9): a new
    // request is not accepted while a prior response awaits indication
    // confirmation. There is only one outbound logical result buffer, so
    // this is silently rejected locally -- no second reply is generated.
    return;
  }

  if (message_type != static_cast<uint8_t>(MessageType::kGetConfigRequest)) {
    buildErrorResponse(correlation_id, ble_app_transport::ErrorCode::kUnsupported,
                        message_type);
    return;
  }
  if (total_length != 0) {
    // GET_CONFIG's frozen wire contract requires a zero-length logical
    // payload. A nonzero length is malformed for this message type; fail
    // closed with no reply and no application/storage side effect.
    return;
  }

  const uint32_t local_request_id = nextLocalRequestId();
  const ApplicationRequest request(ApplicationRequester::kBle,
                                    local_request_id,
                                    ApplicationRequestKind::kGetConfig);
  const auto result = service_.submit(request);
  if (result == ApplicationSubmitResult::kBusy) {
    buildErrorResponse(correlation_id, ble_app_transport::ErrorCode::kBusy,
                        message_type);
    return;
  }
  if (result != ApplicationSubmitResult::kAccepted) {
    // kRejected only occurs for an unsupported requester value; this adapter
    // always submits kBle locally, so this is a defensive fail-closed path
    // for an internal invariant violation, not peer-controlled input.
    return;
  }

  // Take the response into this adapter's own bounded buffer immediately
  // (submit() is synchronous), releasing the global ApplicationRequestService
  // slot right away so a non-reading BLE client can never wedge it, and so
  // USB (or any other requester) remains free to use the shared slot while
  // this response awaits indication (docs/milestones/M7P7F.md section 9).
  ApplicationResponse response;
  if (!service_.takeResponse(ApplicationRequester::kBle, response) ||
      response.request_id != local_request_id) {
    return;  // Defensive; unreachable given synchronous submit()/take above.
  }

  if (response.code != ApplicationResponseCode::kOk) {
    buildErrorResponse(correlation_id, ble_app_transport::ErrorCode::kUnsupported,
                        message_type);
    return;
  }
  buildGetConfigResponse(correlation_id, response);
}

void BleApplicationTransport::buildGetConfigResponse(
    uint16_t correlation_id, const ApplicationResponse& response) {
  outbound_ = OutboundResponse();
  outbound_.pending = true;
  outbound_.correlation_id = correlation_id;
  outbound_.message_type =
      static_cast<uint8_t>(MessageType::kGetConfigResponse);
  outbound_.payload[0] = ble_app_transport::kApplicationStatusOk;
  uint8_t flags = 0;
  if (response.config_backend_ready) {
    flags |= ble_app_transport::kConfigFlagBackendReady;
  }
  if (response.config_has_committed_record) {
    flags |= ble_app_transport::kConfigFlagHasCommittedRecord;
  }
  outbound_.payload[1] = flags;
  writeLE32(outbound_.payload + 2, response.config.tracking_interval_seconds);
  writeLE32(outbound_.payload + 6, response.config.battery_capacity_mah);
  outbound_.total_length = 10;
  outbound_.next_fragment_index = 0;
}

void BleApplicationTransport::buildErrorResponse(
    uint16_t correlation_id, ble_app_transport::ErrorCode code,
    uint8_t offending_message_type) {
  outbound_ = OutboundResponse();
  outbound_.pending = true;
  outbound_.correlation_id = correlation_id;
  outbound_.message_type = static_cast<uint8_t>(MessageType::kError);
  outbound_.payload[0] = static_cast<uint8_t>(code);
  outbound_.payload[1] = offending_message_type;
  outbound_.total_length = 2;
  outbound_.next_fragment_index = 0;
}

uint32_t BleApplicationTransport::nextLocalRequestId() {
  const uint32_t id = next_local_request_id_;
  next_local_request_id_ = ble_app_transport::advanceRequestId(id);
  return id;
}

bool BleApplicationTransport::peekOutboundFrame(uint32_t session_generation,
                                                 uint8_t* frame_out,
                                                 uint8_t& frame_len) const {
  if (!session_active_ || session_generation != session_generation_ ||
      !outbound_.pending || frame_out == nullptr)
    return false;

  const uint16_t offset = static_cast<uint16_t>(outbound_.next_fragment_index *
                                                  kMaxPayloadPerFrame);
  const uint16_t remaining = static_cast<uint16_t>(outbound_.total_length - offset);
  const uint8_t chunk = static_cast<uint8_t>(
      remaining > kMaxPayloadPerFrame ? kMaxPayloadPerFrame : remaining);
  const bool is_first = outbound_.next_fragment_index == 0;
  const bool is_last = (offset + chunk) == outbound_.total_length;

  uint8_t flags = 0;
  if (is_first) flags |= kFlagStart;
  if (is_last) flags |= kFlagEnd;

  frame_out[0] = kTransportVersion;
  frame_out[1] = outbound_.message_type;
  frame_out[2] = flags;
  frame_out[3] = outbound_.next_fragment_index;
  writeLE16(frame_out + 4, outbound_.correlation_id);
  writeLE16(frame_out + 6, outbound_.total_length);
  memcpy(frame_out + kHeaderSize, outbound_.payload + offset, chunk);
  frame_len = static_cast<uint8_t>(kHeaderSize + chunk);
  return true;
}

void BleApplicationTransport::confirmOutboundFrame(
    uint32_t session_generation) {
  if (!session_active_ || session_generation != session_generation_ ||
      !outbound_.pending)
    return;

  const uint16_t offset = static_cast<uint16_t>(outbound_.next_fragment_index *
                                                  kMaxPayloadPerFrame);
  const uint16_t remaining = static_cast<uint16_t>(outbound_.total_length - offset);
  const uint8_t chunk = static_cast<uint8_t>(
      remaining > kMaxPayloadPerFrame ? kMaxPayloadPerFrame : remaining);
  const uint16_t new_offset = static_cast<uint16_t>(offset + chunk);

  if (new_offset >= outbound_.total_length) {
    // Final fragment confirmed: clear the whole logical response now, not
    // before -- a failed/unconfirmed indication must still be retryable
    // with the identical frame via peekOutboundFrame().
    outbound_ = OutboundResponse();
    return;
  }
  outbound_.next_fragment_index =
      static_cast<uint8_t>(outbound_.next_fragment_index + 1);
}

}  // namespace orun_tlp
