// Requested-service intent -> runtime mapping (service_intent.h).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <initializer_list>

#include "config_format.h"
#include "service_intent.h"

using namespace orun_tlp;
namespace cf = config_format;

namespace {

constexpr uint8_t T = cf::kServiceTracking;
constexpr uint8_t R = cf::kServiceRelayForwarding;
constexpr uint8_t A = cf::kServiceApplicationReceive;

ServiceIntentAdmission admitExplicit(uint8_t services) {
  return admitServiceIntent(cf::kServiceModeExplicit, services);
}

void admission() {
  assert(admitServiceIntent(cf::kServiceModeAuto, 0) ==
         ServiceIntentAdmission::kSupported);
  for (unsigned services = 1; services < 256; ++services)
    assert(admitServiceIntent(cf::kServiceModeAuto,
                              static_cast<uint8_t>(services)) ==
           ServiceIntentAdmission::kInvalid);
  for (unsigned mode = 2; mode < 256; ++mode)
    assert(admitServiceIntent(static_cast<uint8_t>(mode), T) ==
           ServiceIntentAdmission::kInvalid);
  for (unsigned bit = 3; bit < 8; ++bit)
    assert(admitExplicit(static_cast<uint8_t>(T | (1u << bit))) ==
           ServiceIntentAdmission::kInvalid);

  // What this runtime can run.
  for (uint8_t services : {uint8_t(0), T, R, uint8_t(T | R), A})
    assert(admitExplicit(services) == ServiceIntentAdmission::kSupported);
  // Valid intent, but receive cannot share a device with relay forwarding
  // (it takes every POSITION) or with tracking (never exercised).
  for (uint8_t services : {uint8_t(A | T), uint8_t(A | R), uint8_t(A | T | R)})
    assert(admitExplicit(services) ==
           ServiceIntentAdmission::kUnsupportedCombination);
}

void legacyCarrier() {
  assert(legacyRoleForServices(A) == NodeRole::kBase);
  assert(legacyRoleForServices(R) == NodeRole::kRelay);
  assert(legacyRoleForServices(T) == NodeRole::kTracker);
  assert(legacyRoleForServices(T | R) == NodeRole::kTracker);
  assert(legacyRoleForServices(0) == NodeRole::kTracker);
}

void requestedConfig() {
  RequestedConfig c = requestedConfigForServices(T);
  assert(c.tracking_enabled && !c.relay_forwarding_enabled &&
         c.location_source == RequestedLocationSource::kGnss);
  assert(validateRequestedConfig(c) == ConfigValidation::kOk);
  c = requestedConfigForServices(T | R);
  assert(c.tracking_enabled && c.relay_forwarding_enabled &&
         c.location_source == RequestedLocationSource::kGnss);
  c = requestedConfigForServices(R);
  assert(!c.tracking_enabled && c.relay_forwarding_enabled &&
         c.location_source == RequestedLocationSource::kNone);
  assert(validateRequestedConfig(c) == ConfigValidation::kOk);
  c = requestedConfigForServices(A);
  assert(!c.tracking_enabled && !c.relay_forwarding_enabled);
  c = requestedConfigForServices(0);
  assert(!c.tracking_enabled && !c.relay_forwarding_enabled);

  // The owner rule: tracking requested and no GNSS blocks tracking; it does
  // not change the device's job (the carrier stays TRACKER).
  const CapabilitySnapshot no_gnss(CapabilityState(
      true, CapabilityPresence::kAbsent, CapabilityHealth::kUnavailable));
  const EffectiveConfig effective =
      resolveRequestedConfig(requestedConfigForServices(T), no_gnss);
  assert(effective.tracking.state == ServiceState::kBlocked &&
         effective.tracking.reason == ServiceReason::kCapabilityAbsent);
  assert(effective.relay_forwarding.state == ServiceState::kDisabled);
  assert(legacyRoleForServices(T) == NodeRole::kTracker);

  const CapabilitySnapshot gnss(CapabilityState(
      true, CapabilityPresence::kPresent, CapabilityHealth::kOk));
  assert(resolveRequestedConfig(requestedConfigForServices(T | R), gnss)
             .tracking.state == ServiceState::kEnabled);
  assert(resolveRequestedConfig(requestedConfigForServices(T | R), no_gnss)
             .relay_forwarding.state == ServiceState::kEnabled);
}

bool parse(const char* text, uint8_t* mode, uint8_t* services) {
  return parseServiceIntent(text, strlen(text), mode, services);
}

void parsing() {
  uint8_t mode = 0xEE, services = 0xEE;
  assert(parse("AUTO", &mode, &services) &&
         mode == cf::kServiceModeAuto && services == 0);
  assert(parse("NONE", &mode, &services) &&
         mode == cf::kServiceModeExplicit && services == 0);
  assert(parse("T", &mode, &services) &&
         mode == cf::kServiceModeExplicit && services == T);
  assert(parse("RT", &mode, &services) && services == (T | R));
  assert(parse("A", &mode, &services) && services == A);
  // Parsing accepts any spelling of the three letters; admission is separate.
  assert(parse("TRA", &mode, &services) && services == (T | R | A));

  mode = 0x55; services = 0x66;
  for (const char* bad : {"", "TT", "X", "t", "auto", "AUTOT", "NONET", "T ",
                          " T", "T,R", "NO"}) {
    assert(!parseServiceIntent(bad, strlen(bad), &mode, &services));
    assert(mode == 0x55 && services == 0x66);
  }
  assert(!parseServiceIntent(nullptr, 1, &mode, &services));
  assert(!parseServiceIntent("T", 1, nullptr, &services));
  assert(!parseServiceIntent("T", 1, &mode, nullptr));
}

void formatting() {
  char out[5];
  assert(strcmp(formatServices(0, out, sizeof(out)), "NONE") == 0);
  assert(strcmp(formatServices(T, out, sizeof(out)), "T") == 0);
  assert(strcmp(formatServices(R | T, out, sizeof(out)), "TR") == 0);
  assert(strcmp(formatServices(A, out, sizeof(out)), "A") == 0);
  assert(strcmp(formatServices(T | R | A, out, sizeof(out)), "TRA") == 0);
  char small[4] = {'x', 'x', 'x', 'x'};
  assert(strcmp(formatServices(T, small, sizeof(small)), "") == 0);
  assert(small[0] == 'x');

  // Every admitted intent round-trips through its USB spelling.
  for (uint8_t services : {uint8_t(0), T, R, uint8_t(T | R), A}) {
    uint8_t mode = 0, parsed = 0xFF;
    formatServices(services, out, sizeof(out));
    assert(parse(out, &mode, &parsed));
    assert(mode == cf::kServiceModeExplicit && parsed == services);
  }
}

}  // namespace

int main() {
  admission();
  legacyCarrier();
  requestedConfig();
  parsing();
  formatting();
  puts("Service intent admission, carrier, parse and format checks: PASS");
  return 0;
}
