#include "ble_write_service.h"

#include <string.h>

#include "custody_ack_primitives.h"

namespace orun_tlp {

namespace {

namespace bat = ble_app_transport;

void putLe16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void putLe32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) { p[i] = static_cast<uint8_t>(v); v >>= 8; }
}
void putBe32(uint8_t* p, uint32_t v) {
  for (int i = 3; i >= 0; --i) { p[i] = static_cast<uint8_t>(v); v >>= 8; }
}
uint32_t getLe32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

constexpr int32_t kMaxLatitudeE7 = 900000000;
constexpr int32_t kMaxLongitudeE7 = 1800000000;

}  // namespace

bool BleWriteService::challenge(uint32_t now, uint8_t* payload,
                                uint16_t& length,
                                bat::ErrorCode& error) {
  length = 0;
  if (!writable_) {
    auth_.reset();
    error = bat::ErrorCode::kAccessDenied;
    return false;
  }
  uint64_t device_id = 0;
  uint32_t epoch = 0;
  uint8_t challenge[ble_write_auth::kChallengeSize]{};
  switch (auth_.issue(now, device_id, epoch, challenge)) {
    case BleChallengeResult::kIssued:
      break;
    case BleChallengeResult::kNoEntropy:
      error = bat::ErrorCode::kBusy;
      return false;
    case BleChallengeResult::kNoCredential:
    default:
      error = bat::ErrorCode::kAccessDenied;
      return false;
  }
  payload[0] = bat::kApplicationStatusOk;
  for (int i = 0; i < 8; ++i)
    payload[1 + i] = static_cast<uint8_t>(device_id >> (56 - 8 * i));
  putLe32(payload + 9, epoch);
  memcpy(payload + 13, challenge, sizeof(challenge));
  length = bat::kWriteChallengePayloadSize;
  return true;
}

bool BleWriteService::stageFence(const uint8_t* payload, uint16_t length,
                                 uint8_t* answer, uint16_t& answer_length,
                                 bat::ErrorCode& error) {
  answer_length = 0;
  if (!writable_) {
    staging_.abort();
    error = bat::ErrorCode::kAccessDenied;
    return false;
  }
  uint8_t result = bat::kFenceStageMalformed;
  if (payload != nullptr && length >= 1) {
    switch (payload[0]) {
      case bat::kFenceStageBegin:
        if (length == 1) {
          staging_.begin();
          result = static_cast<uint8_t>(GeofenceStaging::Result::kOk);
        }
        break;
      case bat::kFenceStageNextArea:
        if (length == 1) result = static_cast<uint8_t>(staging_.nextArea());
        break;
      case bat::kFenceStageAbort:
        if (length == 1) {
          staging_.abort();
          result = static_cast<uint8_t>(GeofenceStaging::Result::kOk);
        }
        break;
      case bat::kFenceStageVertices: {
        const uint8_t n = length >= 2 ? payload[1] : 0;
        if (n == 0 || n > bat::kFenceStageMaxVertices || length != 2 + 8 * n)
          break;
        // Check the whole message first: either all points are staged or
        // none (a range error must not leave half a message behind).
        bool in_range = true;
        for (uint8_t i = 0; i < n; ++i) {
          const int32_t lat = static_cast<int32_t>(getLe32(payload + 2 + 8 * i));
          const int32_t lon = static_cast<int32_t>(getLe32(payload + 6 + 8 * i));
          if (lat < -kMaxLatitudeE7 || lat > kMaxLatitudeE7 ||
              lon < -kMaxLongitudeE7 || lon > kMaxLongitudeE7)
            in_range = false;
        }
        if (!in_range) break;
        if (!staging_.active()) {
          result = static_cast<uint8_t>(GeofenceStaging::Result::kNotStaging);
          break;
        }
        if (staging_.vertexCount() + n > geofence_format::kMaximumTotalVertices) {
          result = static_cast<uint8_t>(GeofenceStaging::Result::kFull);
          break;
        }
        result = static_cast<uint8_t>(GeofenceStaging::Result::kOk);
        for (uint8_t i = 0; i < n; ++i)
          (void)staging_.addVertex(GeoPointE7(
              static_cast<int32_t>(getLe32(payload + 2 + 8 * i)),
              static_cast<int32_t>(getLe32(payload + 6 + 8 * i))));
        break;
      }
      default:
        break;
    }
  }
  answer[0] = result;
  answer[1] = staging_.areaCount();
  putLe16(answer + 2, staging_.vertexCount());
  answer_length = bat::kFenceStageAnswerSize;
  return true;
}

bool BleWriteService::stagedDigest(uint8_t (&digest)[32]) {
  GeofencePolygonView polygons[geofence_format::kMaximumAreas];
  const GeofenceAreaSetView view =
      staging_.active() ? staging_.view(polygons) : GeofenceAreaSetView();
  size_t at = 0;
  canonical_[at++] = static_cast<uint8_t>(view.area_count);
  for (uint16_t a = 0; a < view.area_count; ++a) {
    canonical_[at++] = static_cast<uint8_t>(view.areas[a].vertex_count);
    for (uint16_t v = 0; v < view.areas[a].vertex_count; ++v) {
      putBe32(canonical_ + at,
              static_cast<uint32_t>(view.areas[a].vertices[v].latitude_e7));
      putBe32(canonical_ + at + 4,
              static_cast<uint32_t>(view.areas[a].vertices[v].longitude_e7));
      at += 8;
    }
  }
  const bool ok = custody_crypto::sha256Bounded(canonical_, at, digest);
  memset(canonical_, 0, sizeof(canonical_));
  return ok;
}

BleWriteAdmission BleWriteService::submitWrite(uint32_t now,
                                               uint32_t request_id,
                                               const uint8_t* payload,
                                               uint16_t length) {
  if (!writable_) {
    auth_.reset();
    return BleWriteAdmission::kUnsupported;
  }
  const bool waiting = owed_ != Owed::kNone || immediate_;
  if (payload == nullptr || length != bat::kWriteRequestPayloadSize || waiting) {
    auth_.reset();
    return waiting ? BleWriteAdmission::kBusy : BleWriteAdmission::kDenied;
  }
  const uint8_t op = payload[0];
  const uint32_t value = getLe32(payload + 1);

  // The MAC body: the 5 bytes on the wire; a list commit adds the digest of
  // what this Tracker has staged (so only that exact list can be committed).
  uint8_t body[bat::kWriteRequestBodySize + 32]{};
  size_t body_len = bat::kWriteRequestBodySize;
  memcpy(body, payload, bat::kWriteRequestBodySize);
  if (op == bat::kWriteOpFence) {
    uint8_t digest[32]{};
    if (!stagedDigest(digest)) {
      auth_.reset();
      return BleWriteAdmission::kBusy;
    }
    memcpy(body + body_len, digest, sizeof(digest));
    body_len += sizeof(digest);
  }
  if (!auth_.verify(now, body, body_len, payload + bat::kWriteRequestBodySize))
    return BleWriteAdmission::kDenied;

  // Authenticated from here on: every answer is signed, refusals included.
  Body refused{};
  if (op == bat::kWriteOpFence) {
    const uint8_t areas = staging_.active() ? staging_.areaCount() : 0;
    const uint16_t vertices = staging_.active() ? staging_.vertexCount() : 0;
    geofence_format::StateToken token;
    const bool token_valid = fences_.stateToken(token);
    if (areas == 0 || value != (uint32_t(areas) | (uint32_t(vertices) << 8))) {
      fenceBody(ConfigMutationOutcome::kInvalid, fences_.resourceState(),
                fences_.areaCount(), fences_.totalVertexCount(), token_valid,
                token.revision, refused);
      staging_.abort();  // like USB: a commit ends the candidate either way
      immediate(request_id, refused);
      return BleWriteAdmission::kPending;
    }
    GeofencePolygonView polygons[geofence_format::kMaximumAreas];
    switch (fence_mutations_.submitReplace(
        ApplicationRequester::kBle, request_id,
        ApplicationAccessContext(ApplicationAccessChannel::kBleAuthenticated),
        staging_.view(polygons))) {
      case ConfigMutationSubmitResult::kAccepted:
        staging_.abort();  // the store copied what it needed
        owed_ = Owed::kFence;
        owed_request_id_ = request_id;
        owed_op_ = op;
        return BleWriteAdmission::kPending;
      case ConfigMutationSubmitResult::kBusy:
        auth_.reset();  // staging kept: ask a new challenge, commit again
        return BleWriteAdmission::kBusy;
      case ConfigMutationSubmitResult::kRejected:
      default:
        auth_.reset();
        return BleWriteAdmission::kUnsupported;
    }
  }

  ConfigMutationKind kind = ConfigMutationKind::kSetTrackingInterval;
  uint32_t interval = 0, ble_window = 0;
  uint8_t mode = 0, services = 0;
  bool invalid = false;
  switch (op) {
    case bat::kWriteOpInterval:
      interval = value;
      break;
    case bat::kWriteOpBleWindow:
      kind = ConfigMutationKind::kSetBleWindow;
      ble_window = value;
      break;
    case bat::kWriteOpServices: {
      kind = ConfigMutationKind::kSetServiceIntent;
      mode = static_cast<uint8_t>(value);
      services = static_cast<uint8_t>(value >> 8);
      // Y2a: a Tracker keeps its role; an intent for the other role is
      // refused, never saved (same rule as the USB writer).
      const StorageRole asked = storageRoleForIntent(mode, services);
      invalid = (value >> 16) != 0U ||
                (asked != StorageRole::kUnassigned && asked != role_);
      break;
    }
    default:
      invalid = true;
      break;
  }
  if (invalid) {
    config_format::StateToken token;
    const bool token_valid = store_.stateToken(token);
    configBody(ConfigMutationOutcome::kInvalid, op, store_.config(),
               token_valid, token.revision, refused);
    immediate(request_id, refused);
    return BleWriteAdmission::kPending;
  }

  const ConfigMutationRequest request(
      ApplicationRequester::kBle, request_id,
      ApplicationAccessContext(ApplicationAccessChannel::kBleAuthenticated),
      kind, interval, mode, services, ble_window);
  switch (mutations_.submit(request)) {
    case ConfigMutationSubmitResult::kAccepted:
      owed_ = Owed::kConfig;
      owed_request_id_ = request_id;
      owed_op_ = op;
      return BleWriteAdmission::kPending;
    case ConfigMutationSubmitResult::kBusy:
      auth_.reset();
      return BleWriteAdmission::kBusy;
    case ConfigMutationSubmitResult::kRejected:
    default:
      auth_.reset();
      return BleWriteAdmission::kUnsupported;
  }
}

void BleWriteService::configBody(ConfigMutationOutcome outcome, uint8_t op,
                                 const config_format::Config& config,
                                 bool token_valid, uint32_t revision,
                                 Body& body) const {
  memset(body, 0, sizeof(body));
  body[0] = static_cast<uint8_t>(outcome);
  body[1] = op;
  putLe32(body + 2, config.tracking_interval_seconds);
  putLe16(body + 6, config.ble_window_seconds);
  body[8] = config.service_mode;
  body[9] = config.requested_services;
  putLe32(body + 10, token_valid ? revision : 0U);
}

void BleWriteService::fenceBody(ConfigMutationOutcome outcome,
                                GeofenceResourceState state, uint8_t areas,
                                uint16_t vertices, bool token_valid,
                                uint32_t revision, Body& body) const {
  memset(body, 0, sizeof(body));
  body[0] = static_cast<uint8_t>(outcome);
  body[1] = bat::kWriteOpFence;
  body[2] = static_cast<uint8_t>(state);
  body[3] = areas;
  putLe16(body + 4, vertices);
  putLe32(body + 10, token_valid ? revision : 0U);
}

void BleWriteService::immediate(uint32_t request_id, const Body& body) {
  immediate_ = true;
  immediate_request_id_ = request_id;
  memcpy(immediate_body_, body, sizeof(immediate_body_));
}

void BleWriteService::sessionReset() {
  // The challenge, the staged list and any immediate answer die with the
  // session. A write an owner already holds still finishes; takeAnswer()
  // swallows its result.
  auth_.reset();
  staging_.abort();
  immediate_ = false;
}

BleWriteService::Answer BleWriteService::finish(
    uint32_t request_id, const Body& body,
    uint8_t (&payload)[bat::kWriteResultPayloadSize]) {
  memset(payload, 0, sizeof(payload));
  memcpy(payload, body, sizeof(body));
  (void)request_id;
  uint8_t tag[ble_write_auth::kTagSize]{};
  if (!auth_.signResult(payload, bat::kWriteResultBodySize, tag))
    return Answer::kDropped;
  memcpy(payload + bat::kWriteResultBodySize, tag, sizeof(tag));
  return Answer::kReady;
}

BleWriteService::Answer BleWriteService::takeAnswer(
    uint32_t& request_id, uint8_t (&payload)[bat::kWriteResultPayloadSize]) {
  Body body{};
  if (immediate_) {
    immediate_ = false;
    request_id = immediate_request_id_;
    memcpy(body, immediate_body_, sizeof(body));
    return finish(request_id, body, payload);
  }
  ConfigMutationResult config;
  GeofenceMutationResult fence;
  Owed taken = Owed::kNone;
  if (mutations_.takeResult(ApplicationRequester::kBle, config)) {
    taken = Owed::kConfig;
    request_id = config.request_id;
    configBody(config.outcome, owed_ == Owed::kConfig ? owed_op_ : 0,
               config.config, config.token_valid, config.token.revision, body);
  } else if (fence_mutations_.takeResult(ApplicationRequester::kBle, fence)) {
    taken = Owed::kFence;
    request_id = fence.request_id;
    fenceBody(fence.outcome, fence.state, fence.area_count,
              fence.total_vertex_count, fence.token_valid,
              fence.token.revision, body);
  } else {
    return Answer::kNone;
  }
  if (owed_ != taken || request_id != owed_request_id_) {
    // Not a write this service handed over (cannot normally happen: each
    // owner holds one slot until its result is taken): drop it.
    memset(payload, 0, sizeof(payload));
    memcpy(payload, body, sizeof(body));
    auth_.reset();
    return Answer::kDropped;
  }
  owed_ = Owed::kNone;
  return finish(request_id, body, payload);
}

}  // namespace orun_tlp
