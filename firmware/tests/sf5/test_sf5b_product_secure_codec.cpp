// SF5B PRODUCT_SECURE codec against the independent vectors of
// test_sf5b_product_secure_vectors.py and the decoder rules of
// ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md sections 3-8 and 19.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <functional>
#include <vector>

#include "tlp_v2_product_secure.h"

using namespace orun_tlp::tlp;
using R = ProductSecureCodecResult;

namespace {

// Golden bytes, identical to the pinned hex in the Python vector script.
const char kPeriodicPlaintextHex[] =
    "017f0101a1a2a3a4a5a6a7a8000001016acba47000000e10166adf3e10b8c704"
    "0001e84800ba000c0501010000000ce400000384000e00250fac000101000000";
const char kEventPlaintextHex[] =
    "0101010207000100a1a2a3a4a5a6a7a8000001026acba218b1b2b3b4b5b6b7b8"
    "166b97f010b99dc0001e000000000000";
const char kPeriodicFrameHex[] =
    "0204020103014000112233445566778801020304000000000000004"
    "20a19fe1a180d483149c11e413e3944c95bf8152d9d888ad7b4f16fc0bd0732"
    "07543030490747856b73c6294bec5cac9962ef666a45aeed52a37e70c12790c0"
    "e5283b12ba5d38d03f";
const char kEventFrameHex[] =
    "0204020203013000112233445566778801020304000000000000004"
    "3bbd99aa262fc4aed44203fb2c0a9e6e32815ee72f5b0a03dcc34a22b153b66"
    "ea9269a9a8428c27dbaebfcf615154d9158e90b4be8a6887f5";

std::vector<uint8_t> fromHex(const char* hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; hex[i] != '\0' && hex[i + 1] != '\0'; i += 2) {
    unsigned value = 0;
    assert(sscanf(hex + i, "%2x", &value) == 1);
    out.push_back(static_cast<uint8_t>(value));
  }
  return out;
}

PeriodicObservationPlaintext goldenPeriodic() {
  PeriodicObservationPlaintext p;
  p.record_flags = 0x7F;
  p.location_source = kLocationSourceGnss;
  p.battery_state = 1;  // NORMAL
  p.record_incarnation = 0xA1A2A3A4A5A6A7A8ULL;
  p.record_sequence = 257;
  p.period_end_epoch_seconds = 1791730800UL;
  p.period_duration_seconds = 3600;
  p.latitude_e7 = 376102718;
  p.longitude_e7 = 280545028;
  p.altitude_mm = 125000;
  p.hdop_x100 = 186;
  p.location_age_seconds = 12;
  p.satellites = 5;
  p.activity_quality = kActivityQualityPartial;
  p.battery_quality = kBatteryQualityMeasured;
  p.activity_coverage_seconds = 3300;
  p.active_seconds = 900;
  p.activity_transition_count = 14;
  p.movement_mean_abs_delta_mg = 37;
  p.battery_mv = 4012;
  p.health_flags = kHealthGnssDegraded;
  p.gnss_failure_count = 1;
  return p;
}

EventPlaintext goldenEvent() {
  EventPlaintext e;
  e.event_type = kEventTypeGeofenceOutside;
  e.lifecycle_state = kEventLifecycleActive;
  e.severity = kEventSeverityWarning;
  e.event_flags =
      kEventTransitionTimeValid | kEventLocationValid | kEventLocationAgeValid;
  e.location_source = kLocationSourceGnss;
  e.record_incarnation = 0xA1A2A3A4A5A6A7A8ULL;
  e.record_sequence = 258;
  e.transition_epoch_seconds = 1791730800UL - 600;
  e.occurrence_id = 0xB1B2B3B4B5B6B7B8ULL;
  e.latitude_e7 = 376150000;
  e.longitude_e7 = 280600000;
  e.location_age_seconds = 30;
  return e;
}

void goldenPlaintexts() {
  uint8_t out[kPeriodicPlaintextSize];
  assert(encodePeriodicPlaintext(goldenPeriodic(), out, sizeof(out)) == R::kOk);
  assert(memcmp(out, fromHex(kPeriodicPlaintextHex).data(), sizeof(out)) == 0);
  PeriodicObservationPlaintext decoded;
  assert(decodePeriodicPlaintext(out, sizeof(out), &decoded) == R::kOk);
  uint8_t again[kPeriodicPlaintextSize];
  assert(encodePeriodicPlaintext(decoded, again, sizeof(again)) == R::kOk);
  assert(memcmp(out, again, sizeof(out)) == 0);
  assert(decoded.latitude_e7 == 376102718 && decoded.battery_mv == 4012 &&
         decoded.record_sequence == 257);

  uint8_t event[kEventPlaintextSize];
  assert(encodeEventPlaintext(goldenEvent(), event, sizeof(event)) == R::kOk);
  assert(memcmp(event, fromHex(kEventPlaintextHex).data(), sizeof(event)) == 0);
  EventPlaintext decoded_event;
  assert(decodeEventPlaintext(event, sizeof(event), &decoded_event) == R::kOk);
  assert(decoded_event.occurrence_id == 0xB1B2B3B4B5B6B7B8ULL &&
         decoded_event.location_age_seconds == 30);

  // Negative coordinates survive the signed big-endian round trip.
  PeriodicObservationPlaintext south = goldenPeriodic();
  south.latitude_e7 = -337000000;
  south.longitude_e7 = -1799999999;
  south.altitude_mm = -12;
  assert(encodePeriodicPlaintext(south, out, sizeof(out)) == R::kOk);
  assert(decodePeriodicPlaintext(out, sizeof(out), &decoded) == R::kOk);
  assert(decoded.latitude_e7 == -337000000 &&
         decoded.longitude_e7 == -1799999999 && decoded.altitude_mm == -12);
}

void goldenFrames() {
  const std::vector<uint8_t> periodic = fromHex(kPeriodicFrameHex);
  assert(periodic.size() == kPeriodicFrameSize);
  ProductSecureFrame frame;
  assert(decodeProductSecureFrame(periodic.data(), periodic.size(), &frame) ==
         R::kOk);
  assert(frame.app_family == kProductAppFamilyPeriodic);
  assert(frame.path_flags ==
         (kProductPathFlagRelayAllowed | kProductPathFlagCustodyRequested));
  assert(frame.security_profile == kProductSecurityProfileRootAeadV1);
  assert(frame.ciphertext_len == kPeriodicPlaintextSize);
  assert(frame.device_id == 0x1122334455667788ULL);
  assert(frame.key_epoch == 0x01020304UL);
  assert(frame.security_counter == 0x42);
  assert(memcmp(frame.ciphertext, periodic.data() + 28, 64) == 0);
  assert(memcmp(frame.tag, periodic.data() + 92, 8) == 0);

  uint8_t encoded[kPeriodicFrameSize];
  assert(encodeProductSecureFrame(frame, encoded, sizeof(encoded)) == R::kOk);
  assert(memcmp(encoded, periodic.data(), sizeof(encoded)) == 0);
  uint8_t aad[kProductSecureHeaderSize];
  assert(encodeProductSecureAad(frame, aad) == R::kOk);
  assert(memcmp(aad, periodic.data(), sizeof(aad)) == 0);
  uint8_t nonce[kProductSecureNonceSize];
  productSecureNonce(frame.key_epoch, frame.security_counter, nonce);
  assert(memcmp(nonce, fromHex("01020304010000000000000042").data(),
                sizeof(nonce)) == 0);

  const std::vector<uint8_t> event = fromHex(kEventFrameHex);
  assert(event.size() == kEventFrameSize);
  assert(decodeProductSecureFrame(event.data(), event.size(), &frame) ==
         R::kOk);
  assert(frame.app_family == kProductAppFamilyEvent &&
         frame.ciphertext_len == kEventPlaintextSize &&
         frame.security_counter == 0x43);
  uint8_t encoded_event[kEventFrameSize];
  assert(encodeProductSecureFrame(frame, encoded_event,
                                  sizeof(encoded_event)) == R::kOk);
  assert(memcmp(encoded_event, event.data(), sizeof(encoded_event)) == 0);
  // The encoder needs the family's exact size.
  assert(encodeProductSecureFrame(frame, encoded, sizeof(encoded)) ==
         R::kBadLength);
}

void malformedFrames() {
  const std::vector<uint8_t> golden = fromHex(kPeriodicFrameHex);
  ProductSecureFrame sentinel;
  sentinel.device_id = 0x5A5A;
  auto expect = [&](const std::vector<uint8_t>& bytes, R expected) {
    ProductSecureFrame out = sentinel;
    assert(decodeProductSecureFrame(bytes.data(), bytes.size(), &out) ==
           expected);
    assert(out.device_id == 0x5A5A);  // untouched on failure
  };
  auto mutate = [&](size_t offset, uint8_t value) {
    std::vector<uint8_t> bytes = golden;
    bytes[offset] = value;
    return bytes;
  };
  std::vector<uint8_t> shorter(golden.begin(), golden.end() - 1);
  std::vector<uint8_t> longer = golden;
  longer.push_back(0);
  expect(shorter, R::kBadLength);
  expect(longer, R::kBadLength);
  expect(mutate(0, 0x01), R::kBadVersionOrType);
  expect(mutate(1, 0x03), R::kBadVersionOrType);  // HISTORY_SECURE type
  expect(mutate(7, 0x01), R::kBadReserved);
  expect(mutate(2, 0x01), R::kBadContext);         // BACKEND_A2D
  expect(mutate(4, 0x04), R::kBadPathFlags);       // unknown bit2
  expect(mutate(3, 0x03), R::kBadFamily);
  expect(mutate(3, 0x00), R::kBadFamily);
  expect(mutate(5, 0x02), R::kBadProfile);
  expect(mutate(6, 48), R::kBadCiphertextLength);  // PERIODIC needs 64
  // An EVENT header in a PERIODIC-sized frame: lengths disagree.
  std::vector<uint8_t> event_header = mutate(3, kProductAppFamilyEvent);
  event_header[6] = kEventPlaintextSize;
  expect(event_header, R::kBadLength);
  std::vector<uint8_t> epoch = golden;
  memset(epoch.data() + 16, 0xFF, 4);
  expect(epoch, R::kBadEpoch);
  std::vector<uint8_t> counter = golden;
  memset(counter.data() + 20, 0, 8);
  expect(counter, R::kBadCounter);
  ProductSecureFrame out;
  assert(decodeProductSecureFrame(nullptr, 100, &out) == R::kInvalidArgument);
  assert(decodeProductSecureFrame(golden.data(), golden.size(), nullptr) ==
         R::kInvalidArgument);

  // The encoder and the AAD builder enforce the same header rules.
  ProductSecureFrame frame;
  assert(decodeProductSecureFrame(golden.data(), golden.size(), &frame) ==
         R::kOk);
  uint8_t buffer[kPeriodicFrameSize];
  uint8_t aad[kProductSecureHeaderSize];
  ProductSecureFrame bad = frame;
  bad.security_counter = 0;
  assert(encodeProductSecureFrame(bad, buffer, sizeof(buffer)) ==
         R::kBadCounter);
  assert(encodeProductSecureAad(bad, aad) == R::kBadCounter);
  bad = frame;
  bad.key_epoch = UINT32_MAX;
  assert(encodeProductSecureAad(bad, aad) == R::kBadEpoch);
  bad = frame;
  bad.path_flags = 0x80;
  assert(encodeProductSecureFrame(bad, buffer, sizeof(buffer)) ==
         R::kBadPathFlags);
  assert(encodeProductSecureFrame(frame, nullptr, 100) == R::kInvalidArgument);
}

void periodicRules() {
  auto expect = [](const std::function<void(PeriodicObservationPlaintext&)>& edit,
                   R expected) {
    PeriodicObservationPlaintext p = goldenPeriodic();
    edit(p);
    assert(validatePeriodicPlaintext(p) == expected);
    uint8_t out[kPeriodicPlaintextSize];
    assert(encodePeriodicPlaintext(p, out, sizeof(out)) == expected);
  };
  auto ok = [&](const std::function<void(PeriodicObservationPlaintext&)>& edit) {
    expect(edit, R::kOk);
  };
  auto noLocation = [](PeriodicObservationPlaintext& p) {
    p.record_flags &= static_cast<uint8_t>(
        ~(kPeriodicLocationValid | kPeriodicAltitudeValid |
          kPeriodicLocationAgeValid));
    p.location_source = kLocationSourceUnknown;
    p.latitude_e7 = p.longitude_e7 = p.altitude_mm = 0;
    p.hdop_x100 = 0;
    p.satellites = 0;
    p.location_age_seconds = kLocationAgeUnknown;
  };
  auto noActivity = [](PeriodicObservationPlaintext& p) {
    p.record_flags &= static_cast<uint8_t>(~kPeriodicActivityValid);
    p.activity_coverage_seconds = p.active_seconds = 0;
    p.activity_transition_count = p.movement_mean_abs_delta_mg = 0;
    p.activity_quality = kActivityQualityUnknown;
  };

  expect([](PeriodicObservationPlaintext& p) { p.record_flags |= 0x80; },
         R::kBadFlags);
  expect([](PeriodicObservationPlaintext& p) { p.location_source = 5; },
         R::kBadEnum);
  expect([](PeriodicObservationPlaintext& p) { p.activity_quality = 3; },
         R::kBadEnum);
  expect([](PeriodicObservationPlaintext& p) { p.battery_state = 4; },
         R::kBadEnum);
  expect([](PeriodicObservationPlaintext& p) { p.battery_quality = 3; },
         R::kBadEnum);
  expect([](PeriodicObservationPlaintext& p) { p.record_incarnation = 0; },
         R::kBadIdentity);
  expect([](PeriodicObservationPlaintext& p) { p.record_sequence = 0; },
         R::kBadIdentity);

  // Time.
  expect([](PeriodicObservationPlaintext& p) { p.period_duration_seconds = 0; },
         R::kBadTime);
  expect([](PeriodicObservationPlaintext& p) { p.period_end_epoch_seconds = 0; },
         R::kBadTime);
  expect([](PeriodicObservationPlaintext& p) {
    p.record_flags &= static_cast<uint8_t>(~kPeriodicPeriodTimeValid);
  }, R::kBadTime);
  ok([](PeriodicObservationPlaintext& p) {
    p.record_flags &= static_cast<uint8_t>(~kPeriodicPeriodTimeValid);
    p.period_end_epoch_seconds = 0;
  });

  // Location: canonical "no location" is valid; any leftover is not.
  ok(noLocation);
  expect([&](PeriodicObservationPlaintext& p) {
    noLocation(p);
    p.latitude_e7 = 1;
  }, R::kBadLocation);
  expect([&](PeriodicObservationPlaintext& p) {
    noLocation(p);
    p.satellites = 4;
  }, R::kBadLocation);
  expect([&](PeriodicObservationPlaintext& p) {
    noLocation(p);
    p.location_age_seconds = 12;
  }, R::kBadLocation);
  expect([&](PeriodicObservationPlaintext& p) {
    noLocation(p);
    p.record_flags |= kPeriodicAltitudeValid;
  }, R::kBadLocation);
  expect([](PeriodicObservationPlaintext& p) {
    p.location_source = kLocationSourceUnknown;
  }, R::kBadLocation);
  expect([](PeriodicObservationPlaintext& p) { p.latitude_e7 = 900000001; },
         R::kBadLocation);
  expect([](PeriodicObservationPlaintext& p) { p.longitude_e7 = -1800000001; },
         R::kBadLocation);
  ok([](PeriodicObservationPlaintext& p) {
    p.latitude_e7 = -900000000;
    p.longitude_e7 = 1800000000;
  });
  expect([](PeriodicObservationPlaintext& p) {
    p.record_flags &= static_cast<uint8_t>(~kPeriodicAltitudeValid);
  }, R::kBadLocation);  // altitude kept with its bit cleared
  ok([](PeriodicObservationPlaintext& p) {
    p.record_flags &= static_cast<uint8_t>(~kPeriodicAltitudeValid);
    p.altitude_mm = 0;
  });
  expect([](PeriodicObservationPlaintext& p) {
    p.location_age_seconds = kLocationAgeUnknown;
  }, R::kBadLocation);
  ok([](PeriodicObservationPlaintext& p) {
    p.location_age_seconds = kLocationAgeSaturated;
  });
  expect([](PeriodicObservationPlaintext& p) {
    p.record_flags &= static_cast<uint8_t>(~kPeriodicLocationAgeValid);
  }, R::kBadLocation);
  expect([](PeriodicObservationPlaintext& p) {
    p.location_source = kLocationSourcePhone;  // keeps GNSS hdop/satellites
  }, R::kBadLocation);
  ok([](PeriodicObservationPlaintext& p) {
    p.location_source = kLocationSourcePhone;
    p.hdop_x100 = 0;
    p.satellites = 0;
  });
  ok([](PeriodicObservationPlaintext& p) {
    p.hdop_x100 = 0;  // GNSS with quality unavailable
    p.satellites = 0;
  });

  // Activity.
  ok(noActivity);
  expect([&](PeriodicObservationPlaintext& p) {
    noActivity(p);
    p.movement_mean_abs_delta_mg = 1;
  }, R::kBadActivity);
  expect([&](PeriodicObservationPlaintext& p) {
    noActivity(p);
    p.record_flags |= kPeriodicActivityValid;
  }, R::kBadActivity);  // valid with zero coverage
  expect([](PeriodicObservationPlaintext& p) { p.active_seconds = 3301; },
         R::kBadActivity);
  expect([](PeriodicObservationPlaintext& p) {
    p.activity_coverage_seconds = 3601;
  }, R::kBadActivity);
  expect([](PeriodicObservationPlaintext& p) {
    p.activity_coverage_seconds = 3600;  // complete, still labelled PARTIAL
  }, R::kBadActivity);
  ok([](PeriodicObservationPlaintext& p) {
    p.activity_coverage_seconds = 3600;
    p.activity_quality = kActivityQualityComplete;
  });
  expect([](PeriodicObservationPlaintext& p) {
    p.activity_quality = kActivityQualityComplete;  // partial coverage
  }, R::kBadActivity);
  ok([](PeriodicObservationPlaintext& p) {
    p.activity_transition_count = 0xFFFF;  // saturated
    p.movement_mean_abs_delta_mg = 0xFFFF;
  });

  // Battery.
  expect([](PeriodicObservationPlaintext& p) {
    p.record_flags &= static_cast<uint8_t>(~kPeriodicBatteryValid);
  }, R::kBadBattery);
  ok([](PeriodicObservationPlaintext& p) {
    p.record_flags &= static_cast<uint8_t>(~kPeriodicBatteryValid);
    p.battery_mv = 0;
    p.battery_state = kBatteryStateUnknown;
    p.battery_quality = kBatteryQualityUnknown;
  });

  // Health.
  expect([](PeriodicObservationPlaintext& p) { p.health_flags = 1U << 11; },
         R::kBadHealth);
  expect([](PeriodicObservationPlaintext& p) {
    p.record_flags &= static_cast<uint8_t>(~kPeriodicHealthValid);
  }, R::kBadHealth);
  ok([](PeriodicObservationPlaintext& p) {
    p.record_flags &= static_cast<uint8_t>(~kPeriodicHealthValid);
    p.health_flags = 0;
    p.gnss_failure_count = 0;
  });
  ok([](PeriodicObservationPlaintext& p) {
    p.health_flags = kHealthFlagsAllowedMask;
    p.radio_failure_count = 255;
  });

  // Decoder-only checks.
  std::vector<uint8_t> bytes = fromHex(kPeriodicPlaintextHex);
  PeriodicObservationPlaintext out;
  out.record_sequence = 0x5A5A;
  bytes[0] = 0x02;
  assert(decodePeriodicPlaintext(bytes.data(), bytes.size(), &out) ==
         R::kBadSchema);
  bytes = fromHex(kPeriodicPlaintextHex);
  bytes[43] = 1;
  assert(decodePeriodicPlaintext(bytes.data(), bytes.size(), &out) ==
         R::kBadReserved);
  bytes = fromHex(kPeriodicPlaintextHex);
  bytes[1] = 0x77;  // LOCATION_AGE_VALID dropped, age 12 kept
  assert(decodePeriodicPlaintext(bytes.data(), bytes.size(), &out) ==
         R::kBadLocation);
  assert(decodePeriodicPlaintext(bytes.data(), 63, &out) == R::kBadLength);
  assert(decodePeriodicPlaintext(nullptr, 64, &out) == R::kInvalidArgument);
  assert(out.record_sequence == 0x5A5A);
  uint8_t small[63];
  assert(encodePeriodicPlaintext(goldenPeriodic(), small, sizeof(small)) ==
         R::kBadLength);
  assert(encodePeriodicPlaintext(goldenPeriodic(), nullptr, 64) ==
         R::kInvalidArgument);
}

void eventRules() {
  auto expect = [](const std::function<void(EventPlaintext&)>& edit,
                   R expected) {
    EventPlaintext e = goldenEvent();
    edit(e);
    assert(validateEventPlaintext(e) == expected);
    uint8_t out[kEventPlaintextSize];
    assert(encodeEventPlaintext(e, out, sizeof(out)) == expected);
  };
  auto ok = [&](const std::function<void(EventPlaintext&)>& edit) {
    expect(edit, R::kOk);
  };

  expect([](EventPlaintext& e) { e.event_type = 0; }, R::kBadEnum);
  expect([](EventPlaintext& e) { e.event_type = 5; }, R::kBadEnum);
  expect([](EventPlaintext& e) { e.lifecycle_state = 3; }, R::kBadEnum);
  expect([](EventPlaintext& e) { e.severity = 0; }, R::kBadEnum);
  expect([](EventPlaintext& e) { e.severity = 4; }, R::kBadEnum);
  expect([](EventPlaintext& e) { e.location_source = 5; }, R::kBadEnum);
  expect([](EventPlaintext& e) { e.context_kind = 3; }, R::kBadEnum);
  expect([](EventPlaintext& e) { e.reason_code = 1; }, R::kBadEnum);
  ok([](EventPlaintext& e) { e.lifecycle_state = kEventLifecycleCleared; });

  // Battery events carry LOW or CRITICAL and may carry the voltage.
  auto battery = [](EventPlaintext& e) {
    e.event_type = kEventTypeBatteryState;
    e.severity = kEventSeverityCritical;
    e.reason_code = kBatteryReasonCritical;
    e.event_flags |= kEventContextValid;
    e.context_kind = kEventContextBatteryMv;
    e.context_value = 3320;
  };
  ok(battery);
  expect([&](EventPlaintext& e) {
    battery(e);
    e.reason_code = 0;
  }, R::kBadEnum);
  expect([&](EventPlaintext& e) {
    battery(e);
    e.reason_code = 3;
  }, R::kBadEnum);

  expect([](EventPlaintext& e) { e.event_flags |= 0x10; }, R::kBadFlags);
  expect([](EventPlaintext& e) { e.occurrence_id = 0; }, R::kBadIdentity);
  expect([](EventPlaintext& e) { e.record_sequence = 0; }, R::kBadIdentity);
  expect([](EventPlaintext& e) { e.transition_epoch_seconds = 0; },
         R::kBadTime);
  ok([](EventPlaintext& e) {
    e.event_flags &= static_cast<uint8_t>(~kEventTransitionTimeValid);
    e.transition_epoch_seconds = 0;
  });

  auto noLocation = [](EventPlaintext& e) {
    e.event_flags &=
        static_cast<uint8_t>(~(kEventLocationValid | kEventLocationAgeValid));
    e.location_source = kLocationSourceUnknown;
    e.latitude_e7 = e.longitude_e7 = 0;
    e.location_age_seconds = kLocationAgeUnknown;
  };
  ok(noLocation);
  expect([&](EventPlaintext& e) {
    noLocation(e);
    e.longitude_e7 = 5;
  }, R::kBadLocation);
  expect([&](EventPlaintext& e) {
    noLocation(e);
    e.event_flags |= kEventLocationAgeValid;  // age without location
    e.location_age_seconds = 3;
  }, R::kBadLocation);
  expect([](EventPlaintext& e) { e.location_age_seconds = kLocationAgeUnknown; },
         R::kBadLocation);
  ok([](EventPlaintext& e) {
    e.event_flags &= static_cast<uint8_t>(~kEventLocationAgeValid);
    e.location_age_seconds = kLocationAgeUnknown;
  });
  expect([](EventPlaintext& e) { e.latitude_e7 = -900000001; },
         R::kBadLocation);

  expect([](EventPlaintext& e) { e.context_flags = 1; }, R::kBadContextValue);
  expect([](EventPlaintext& e) { e.context_value = 7; }, R::kBadContextValue);
  expect([](EventPlaintext& e) { e.context_kind = kEventContextSubsystemId; },
         R::kBadContextValue);
  expect([](EventPlaintext& e) { e.event_flags |= kEventContextValid; },
         R::kBadContextValue);

  std::vector<uint8_t> bytes = fromHex(kEventPlaintextHex);
  EventPlaintext out;
  out.record_sequence = 0x5A5A;
  bytes[0] = 0x00;
  assert(decodeEventPlaintext(bytes.data(), bytes.size(), &out) ==
         R::kBadSchema);
  bytes = fromHex(kEventPlaintextHex);
  bytes[5] = 0x01;  // reason code on a geofence event
  assert(decodeEventPlaintext(bytes.data(), bytes.size(), &out) == R::kBadEnum);
  assert(decodeEventPlaintext(bytes.data(), 47, &out) == R::kBadLength);
  assert(out.record_sequence == 0x5A5A);
}

void names() {
  for (unsigned value = 0;
       value <= static_cast<unsigned>(R::kBadContextValue); ++value) {
    const char* name = productSecureCodecResultName(static_cast<R>(value));
    assert(name != nullptr && strcmp(name, "UNKNOWN") != 0);
  }
  assert(strcmp(productSecureCodecResultName(R::kOk), "OK") == 0);
}

}  // namespace

int main() {
  goldenPlaintexts();
  goldenFrames();
  malformedFrames();
  periodicRules();
  eventRules();
  names();
  puts("SF5B PRODUCT_SECURE codec golden/malformed/semantic checks: PASS");
  return 0;
}
