#include <assert.h>
#include <stdio.h>

#include "node_role.h"
#include "runtime_config.h"

using namespace orun_tlp;

namespace {

CapabilitySnapshot gnss(bool supported, CapabilityPresence presence,
                        CapabilityHealth health) {
  return CapabilitySnapshot(CapabilityState(supported, presence, health));
}

void assertStatus(const ServiceStatus& status, ServiceState state,
                  ServiceReason reason) {
  assert(status.state == state);
  assert(status.reason == reason);
}

}  // namespace

int main() {
  const RequestedConfig tracker =
      requestedConfigFromLegacyBehavior(legacyRoleBehavior(NodeRole::kTracker));
  assert(tracker.tracking_enabled);
  assert(!tracker.relay_forwarding_enabled);
  assert(tracker.location_source == RequestedLocationSource::kGnss);

  const RequestedConfig relay =
      requestedConfigFromLegacyBehavior(legacyRoleBehavior(NodeRole::kRelay));
  assert(!relay.tracking_enabled);
  assert(relay.relay_forwarding_enabled);
  assert(relay.location_source == RequestedLocationSource::kNone);

  const RequestedConfig base =
      requestedConfigFromLegacyBehavior(legacyRoleBehavior(NodeRole::kBase));
  assert(!base.tracking_enabled);
  assert(!base.relay_forwarding_enabled);
  assert(base.location_source == RequestedLocationSource::kNone);

  assert(validateRequestedConfig(RequestedConfig(true, false,
      RequestedLocationSource::kNone)) ==
      ConfigValidation::kTrackingRequiresLocationSource);
  assert(validateRequestedConfig(RequestedConfig(true, false,
      RequestedLocationSource::kGnss)) == ConfigValidation::kOk);
  assert(validateRequestedConfig(RequestedConfig(false, false,
      RequestedLocationSource::kGnss)) == ConfigValidation::kOk);
  assert(validateRequestedConfig(RequestedConfig(false, true,
      RequestedLocationSource::kNone)) == ConfigValidation::kOk);

  const RequestedLocationSource unknown_source =
      static_cast<RequestedLocationSource>(2);
  assert(validateRequestedConfig(RequestedConfig(false, true, unknown_source)) ==
         ConfigValidation::kInvalidLocationSource);
  assert(validateRequestedConfig(RequestedConfig(true, true, unknown_source)) ==
         ConfigValidation::kInvalidLocationSource);

  const RequestedConfig tracking_and_relay(
      true, true, RequestedLocationSource::kGnss);
  const RequestedConfig before = tracking_and_relay;
  EffectiveConfig effective = resolveRequestedConfig(
      tracking_and_relay,
      gnss(true, CapabilityPresence::kPresent, CapabilityHealth::kOk));
  assertStatus(effective.tracking, ServiceState::kEnabled,
               ServiceReason::kNone);
  assertStatus(effective.relay_forwarding, ServiceState::kEnabled,
               ServiceReason::kNone);
  assert(tracking_and_relay.tracking_enabled == before.tracking_enabled);
  assert(tracking_and_relay.relay_forwarding_enabled ==
         before.relay_forwarding_enabled);
  assert(tracking_and_relay.location_source == before.location_source);

  effective = resolveRequestedConfig(
      tracking_and_relay,
      gnss(true, CapabilityPresence::kUnknown,
           CapabilityHealth::kUnavailable));
  assertStatus(effective.tracking, ServiceState::kBlocked,
               ServiceReason::kCapabilityUnknown);
  assertStatus(effective.relay_forwarding, ServiceState::kEnabled,
               ServiceReason::kNone);

  effective = resolveRequestedConfig(
      tracking_and_relay,
      gnss(true, CapabilityPresence::kAbsent,
           CapabilityHealth::kUnavailable));
  assertStatus(effective.tracking, ServiceState::kBlocked,
               ServiceReason::kCapabilityAbsent);

  effective = resolveRequestedConfig(
      tracking_and_relay,
      gnss(true, CapabilityPresence::kPresent,
           CapabilityHealth::kDegraded));
  assertStatus(effective.tracking, ServiceState::kDegraded,
               ServiceReason::kCapabilityDegraded);

  effective = resolveRequestedConfig(
      tracking_and_relay,
      gnss(true, CapabilityPresence::kPresent, CapabilityHealth::kFault));
  assertStatus(effective.tracking, ServiceState::kBlocked,
               ServiceReason::kCapabilityFault);

  effective = resolveRequestedConfig(
      tracking_and_relay,
      gnss(true, CapabilityPresence::kPresent,
           CapabilityHealth::kUnavailable));
  assertStatus(effective.tracking, ServiceState::kBlocked,
               ServiceReason::kCapabilityUnavailable);

  effective = resolveRequestedConfig(
      tracking_and_relay,
      gnss(false, CapabilityPresence::kUnknown,
           CapabilityHealth::kUnavailable));
  assertStatus(effective.tracking, ServiceState::kBlocked,
               ServiceReason::kCapabilityUnsupported);

  const RequestedConfig invalid(true, true, RequestedLocationSource::kNone);
  effective = resolveRequestedConfig(
      invalid,
      gnss(true, CapabilityPresence::kPresent, CapabilityHealth::kOk));
  assertStatus(effective.tracking, ServiceState::kBlocked,
               ServiceReason::kInvalidConfiguration);
  assertStatus(effective.relay_forwarding, ServiceState::kBlocked,
               ServiceReason::kInvalidConfiguration);

  const RequestedConfig unknown_source_relay(false, true, unknown_source);
  effective = resolveRequestedConfig(
      unknown_source_relay,
      gnss(true, CapabilityPresence::kPresent, CapabilityHealth::kOk));
  assertStatus(effective.tracking, ServiceState::kBlocked,
               ServiceReason::kInvalidConfiguration);
  assertStatus(effective.relay_forwarding, ServiceState::kBlocked,
               ServiceReason::kInvalidConfiguration);

  const RequestedConfig unknown_source_tracking(true, true, unknown_source);
  effective = resolveRequestedConfig(
      unknown_source_tracking,
      gnss(true, CapabilityPresence::kPresent, CapabilityHealth::kOk));
  assertStatus(effective.tracking, ServiceState::kBlocked,
               ServiceReason::kInvalidConfiguration);
  assertStatus(effective.relay_forwarding, ServiceState::kBlocked,
               ServiceReason::kInvalidConfiguration);

  const RequestedConfig disabled(false, false,
                                 RequestedLocationSource::kNone);
  effective = resolveRequestedConfig(
      disabled,
      gnss(true, CapabilityPresence::kPresent, CapabilityHealth::kOk));
  assertStatus(effective.tracking, ServiceState::kDisabled,
               ServiceReason::kNotRequested);
  assertStatus(effective.relay_forwarding, ServiceState::kDisabled,
               ServiceReason::kNotRequested);

  puts("B4 requested/capability/effective config model: PASS");
}
