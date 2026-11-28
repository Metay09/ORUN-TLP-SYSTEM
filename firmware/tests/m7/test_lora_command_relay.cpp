// 6b: the Gateway's LoRa command mailbox (lora_command_relay.h).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "lora_command_relay.h"

using namespace orun_tlp;
namespace lc = orun_tlp::lora_command;

namespace {

std::vector<uint8_t> command(uint64_t target, uint32_t id, uint8_t n = 1) {
  std::vector<uint8_t> f = {lc::kVersion, lc::kCommandType, 0, lc::kResourceSettings};
  for (int i = 7; i >= 0; --i) f.push_back(uint8_t(target >> (8 * i)));
  for (int i = 0; i < 4; ++i) f.push_back(0);  // epoch
  for (int i = 3; i >= 0; --i) f.push_back(uint8_t(id >> (8 * i)));
  for (int i = 0; i < 12; ++i) f.push_back(0);  // token
  f.push_back(n);
  for (uint8_t e = 0; e < n; ++e) {
    f.push_back(1);
    for (int i = 0; i < 4; ++i) f.push_back(0);
  }
  for (int i = 0; i < 16; ++i) f.push_back(0xAA);  // tag (not checked here)
  return f;
}

// Area list parts (shape only; contents are not looked at by the Gateway).
std::vector<std::vector<uint8_t>> areaParts(uint64_t target, uint32_t id, uint8_t areas,
                                            uint16_t vertices) {
  const size_t total = lc::areaSetSize(areas, vertices);
  const size_t parts = (total + lc::kAreaChunk - 1) / lc::kAreaChunk;
  std::vector<std::vector<uint8_t>> out;
  for (size_t p = 0; p < parts; ++p) {
    std::vector<uint8_t> f = {lc::kVersion, lc::kCommandType, 0, lc::kResourceAreas};
    for (int i = 7; i >= 0; --i) f.push_back(uint8_t(target >> (8 * i)));
    for (int i = 0; i < 4; ++i) f.push_back(0);
    for (int i = 3; i >= 0; --i) f.push_back(uint8_t(id >> (8 * i)));
    for (int i = 0; i < 12; ++i) f.push_back(0);
    f.push_back(uint8_t(p));
    f.push_back(uint8_t(parts));
    f.push_back(areas);
    f.push_back(uint8_t(vertices >> 8));
    f.push_back(uint8_t(vertices));
    const size_t chunk = p + 1 < parts ? lc::kAreaChunk : total - p * lc::kAreaChunk;
    for (size_t i = 0; i < chunk; ++i) f.push_back(uint8_t(p));
    for (int i = 0; i < 16; ++i) f.push_back(0xAA);
    out.push_back(f);
  }
  return out;
}

std::vector<uint8_t> result(uint64_t device, uint32_t id) {
  std::vector<uint8_t> r(lc::kResultSize, 0);
  r[0] = lc::kVersion;
  r[1] = lc::kResultType;
  for (int i = 0; i < 8; ++i) r[4 + i] = uint8_t(device >> (8 * (7 - i)));
  for (int i = 0; i < 4; ++i) r[16 + i] = uint8_t(id >> (8 * (3 - i)));
  return r;
}

constexpr uint64_t kA = 0x0E8ADE7E71531AA3ULL, kB = 0x52E16773141A8887ULL;

void testWaitsForItsTrackerAndTheAck() {
  LoraCommandRelay relay;
  const auto a = command(kA, 7);
  assert(relay.queue(a.data(), a.size()) == LoraCommandRelay::Queued::kAdded);
  LoraCommandRelay::Frame f{};
  assert(!relay.trySend(1000, f, nullptr, [](const LoraCommandRelay::Frame&) { return true; }));  // nobody heard yet
  relay.heard(kB, 1000);        // another Tracker: nothing for it
  assert(!relay.trySend(4000, f, nullptr, [](const LoraCommandRelay::Frame&) { return true; }));
  relay.heard(kA, 10000);
  assert(!relay.trySend(10100, f, nullptr, [](const LoraCommandRelay::Frame&) { return true; }));  // its custody ACK goes first
  relay.ackSent(11000);
  assert(relay.trySend(11000, f, nullptr, [](const LoraCommandRelay::Frame&) { return true; }));
  assert(f.target == kA && f.command_id == 7 && f.attempt == 1 && f.size == a.size() &&
         memcmp(f.bytes, a.data(), a.size()) == 0);
  assert(!relay.trySend(11100, f, nullptr, [](const LoraCommandRelay::Frame&) { return true; }));  // one try per report
  // No ACK at all: sent after the grace time, but not after the window.
  relay.heard(kA, 20000);
  assert(!relay.trySend(20000 + LoraCommandRelay::kAckGraceMs - 1, f, nullptr, [](const LoraCommandRelay::Frame&) { return true; }));
  assert(relay.trySend(20000 + LoraCommandRelay::kAckGraceMs, f, nullptr, [](const LoraCommandRelay::Frame&) { return true; }) && f.attempt == 2);
  relay.heard(kA, 30000);
  assert(!relay.trySend(30000 + LoraCommandRelay::kLatestAfterHeardMs, f, nullptr, [](const LoraCommandRelay::Frame&) { return true; }));
  // The answer removes it; an answer for another id does not.
  assert(!relay.answered(result(kA, 8).data(), lc::kResultSize));
  assert(relay.count() == 1);
  assert(relay.answered(result(kA, 7).data(), lc::kResultSize));
  assert(relay.count() == 0);
}

void testReplaceFullMalformed() {
  LoraCommandRelay relay;
  const auto a1 = command(kA, 1), a2 = command(kA, 2, 3);
  assert(relay.queue(a1.data(), a1.size()) == LoraCommandRelay::Queued::kAdded);
  assert(relay.queue(a2.data(), a2.size()) == LoraCommandRelay::Queued::kReplaced);
  relay.heard(kA, 0);
  relay.ackSent(0);
  LoraCommandRelay::Frame f{};
  assert(relay.trySend(0, f, nullptr, [](const LoraCommandRelay::Frame&) { return true; }) && f.command_id == 2 && f.size == a2.size());
  for (uint64_t t = 1; t < LoraCommandRelay::kSlots; ++t) {
    const auto c = command(t, 1);
    assert(relay.queue(c.data(), c.size()) == LoraCommandRelay::Queued::kAdded);
  }
  const auto extra = command(999, 1);
  assert(relay.queue(extra.data(), extra.size()) == LoraCommandRelay::Queued::kFull);
  auto bad = command(kB, 1);
  bad.pop_back();
  assert(relay.queue(bad.data(), bad.size()) == LoraCommandRelay::Queued::kMalformed);
  bad = command(kB, 1);
  bad[1] = lc::kResultType;
  assert(relay.queue(bad.data(), bad.size()) == LoraCommandRelay::Queued::kMalformed);
  assert(relay.queue(nullptr, 0) == LoraCommandRelay::Queued::kMalformed);
}

void testGivesUpAfterMaxAttempts() {
  LoraCommandRelay relay;
  const auto a = command(kA, 5);
  relay.queue(a.data(), a.size());
  LoraCommandRelay::Frame f{};
  uint64_t target = 0;
  uint32_t id = 0;
  uint32_t t = 0;
  for (uint8_t i = 1; i <= LoraCommandRelay::kMaxAttempts; ++i) {
    t += 60000;
    relay.heard(kA, t);
    relay.ackSent(t);
    assert(!relay.takeDropped(target, id));
    assert(relay.trySend(t, f, nullptr, [](const LoraCommandRelay::Frame&) { return true; }) && f.attempt == i);
  }
  // Heard again with no answer to the last try: dropped, and said so once.
  t += 60000;
  relay.heard(kA, t);
  assert(relay.takeDropped(target, id) && target == kA && id == 5);
  assert(!relay.takeDropped(target, id));
  assert(!relay.trySend(t, f, nullptr, [](const LoraCommandRelay::Frame&) { return true; }) && relay.count() == 0);
}

void testAreaPartsGoBackToBack() {
  LoraCommandRelay relay;
  auto parts = areaParts(kA, 9, 8, 64);
  assert(parts.size() == 3 && parts[0].size() == lc::kMaxFrameSize);
  // Parts may arrive from the bridge in any order; a copy is harmless.
  assert(relay.queue(parts[1].data(), parts[1].size()) == LoraCommandRelay::Queued::kAdded);
  assert(relay.queue(parts[0].data(), parts[0].size()) == LoraCommandRelay::Queued::kReplaced);
  assert(relay.queue(parts[2].data(), parts[2].size()) == LoraCommandRelay::Queued::kReplaced);
  assert(relay.queue(parts[2].data(), parts[2].size()) == LoraCommandRelay::Queued::kReplaced);
  assert(relay.count() == 1);
  relay.heard(kA, 1000);
  relay.ackSent(1500);
  LoraCommandRelay::Frame f{};
  for (uint8_t p = 0; p < 3; ++p) {
    assert(relay.trySend(1500 + p * 6000, f, nullptr, [](const LoraCommandRelay::Frame&) { return true; }));
    assert(f.part == p && f.parts == 3 && f.attempt == 1 && f.size == parts[p].size() &&
           memcmp(f.bytes, parts[p].data(), f.size) == 0);
           relay.onTxResult(f.target, f.command_id, f.part, true);
  }
  assert(!relay.trySend(20000, f, nullptr, [](const LoraCommandRelay::Frame&) { return true; }));  // all three went in this window
  // Next report: all three again, attempt 2.
  relay.heard(kA, 60000);
  relay.ackSent(60000);
  assert(relay.trySend(60000, f, nullptr, [](const LoraCommandRelay::Frame&) { return true; }) && f.part == 0 && f.attempt == 2);
  // A newer command for the same Tracker replaces every part.
  auto clear = areaParts(kA, 10, 0, 0);
  assert(clear.size() == 1 && clear[0].size() == lc::kMinFrameSize);
  assert(relay.queue(clear[0].data(), clear[0].size()) == LoraCommandRelay::Queued::kReplaced);
  relay.heard(kA, 120000);
  relay.ackSent(120000);
  assert(relay.trySend(120000, f, nullptr, [](const LoraCommandRelay::Frame&) { return true; }) && f.command_id == 10 && f.parts == 1 && f.attempt == 1);
  assert(!relay.trySend(120100, f, nullptr, [](const LoraCommandRelay::Frame&) { return true; }));
  // A part whose sizes do not add up is refused.
  auto bad = areaParts(kB, 11, 1, 6);
  bad[0].pop_back();
  assert(relay.queue(bad[0].data(), bad[0].size()) == LoraCommandRelay::Queued::kMalformed);
}


void testTerminalAndReplacement() {
  LoraCommandRelay relay;

  const auto parts = areaParts(kA, 100U, 8U, 64U);
  const auto newer = areaParts(kA, 101U, 8U, 64U);

  for (const auto& part : parts)
    relay.queue(part.data(), part.size());

  LoraCommandRelay::Frame f{};

  auto send = [&](uint32_t now) {
    return relay.trySend(
        now, f, nullptr,
        [](const LoraCommandRelay::Frame&) {
          return true;
        });
  };

  relay.heard(kA, 1000U);

  relay.ackSentFor(kB, 1000U);
  assert(!send(1100U));

  relay.ackSentFor(kA, 900U);
  assert(!send(1100U));

  relay.ackSentFor(kA, 1000U);

  assert(send(3500U));
  assert(f.part == 0U && f.attempt == 1U);

  assert(!send(9000U));

  relay.onTxResult(kA, 999U, 0U, true);
  assert(!send(9000U));

  relay.onTxResult(kA, 100U, 1U, true);
  assert(!send(9000U));

  relay.onTxResult(kA, 100U, 0U, false);

  assert(!send(9500U));
  assert(!send(12000U));
  assert(relay.count() == 1U);

  relay.heard(kA, 20000U);
  relay.ackSentFor(kA, 20000U);

  assert(send(20000U));
  assert(f.part == 0U && f.attempt == 2U);

  relay.queue(newer[1].data(), newer[1].size());
  relay.queue(newer[2].data(), newer[2].size());

  relay.onTxResult(kA, 100U, 0U, true);
  assert(!send(26000U));

  relay.heard(kA, 40000U);
  relay.ackSentFor(kA, 40000U);

  assert(!send(40000U));

  relay.queue(newer[0].data(), newer[0].size());

  assert(send(40000U));
  assert(f.command_id == 101U);
  assert(f.part == 0U && f.attempt == 1U);

  relay.onTxResult(kA, 100U, 0U, true);
  assert(!send(46000U));

  relay.onTxResult(kA, 101U, 0U, true);

  assert(send(46000U));
  assert(f.part == 1U && f.attempt == 1U);

  puts("ORUN-305 P1-2 TERMINAL AND TIMEOUT PASS");
  puts("ORUN-305 P1-3 REPLACEMENT PASS");
  puts("ORUN-305 ACK PEER AND WINDOW MATCH PASS");
}

}  // namespace

int main() {
  testTerminalAndReplacement();
  testWaitsForItsTrackerAndTheAck();
  testReplaceFullMalformed();
  testGivesUpAfterMaxAttempts();
  testAreaPartsGoBackToBack();
  puts("6b Gateway LoRa command relay (wait for Tracker and ACK, one try per report, "
       "replace, full, malformed, give up, area parts back to back): PASS");
  return 0;
}
