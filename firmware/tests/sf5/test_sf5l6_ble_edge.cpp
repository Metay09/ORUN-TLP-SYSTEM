// SF5L6 BLE Edge link, host only.
// edge_link framing/messages + Sf5l0EdgeLinkSession over the REAL custody
// store/owner/ingress and Sf5l0EdgeHandoff (same accept owner as USB).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "edge_link.h"

#define ORUN_SF5L5_NO_MAIN
#include "test_sf5l5_edge_handoff.cpp"

namespace {
namespace el = orun_tlp::edge_link;

void testFraming() {
  uint8_t msg[el::kMaxMessage];
  for (size_t i = 0; i < sizeof(msg); ++i) msg[i] = uint8_t(i * 7U + 3U);
  for (size_t size = 1; size <= el::kMaxMessage; ++size) {
    el::Fragmenter f;
    el::Reassembler r;
    assert(f.load(msg, size, uint8_t(size)));
    unsigned frames = 0;
    el::Reassembler::Result last = el::Reassembler::Result::kNeedMore;
    while (f.pending()) {
      uint8_t frame[el::kFrameSize];
      uint8_t len = 0;
      assert(f.peek(frame, len) && len <= el::kFrameSize);
      uint8_t again[el::kFrameSize];
      uint8_t len2 = 0;
      assert(f.peek(again, len2) && len2 == len && memcmp(frame, again, len) == 0);
      last = r.push(frame, len, 0);
      f.advance();
      ++frames;
    }
    assert(last == el::Reassembler::Result::kComplete);
    assert(r.size() == size && memcmp(r.message(), msg, size) == 0);
    assert(frames == (size + el::kFrameData - 1) / el::kFrameData);
  }
  el::Fragmenter big;
  assert(!big.load(msg, el::kMaxMessage + 1, 1) && !big.pending());
  assert(!big.load(msg, 0, 1));

  // A missing middle frame, a foreign message id or a bad version drops the
  // partial message; a later complete message still goes through.
  el::Fragmenter f;
  assert(f.load(msg, 40, 9));
  uint8_t a[20], b[20], c[20];
  uint8_t la, lb, lc;
  f.peek(a, la); f.advance();
  f.peek(b, lb); f.advance();
  f.peek(c, lc); f.advance();
  el::Reassembler r;
  assert(r.push(a, la, 0) == el::Reassembler::Result::kNeedMore);
  assert(r.push(c, lc, 0) == el::Reassembler::Result::kDropped);
  assert(r.size() == 0U);
  assert(r.push(a, la, 0) == el::Reassembler::Result::kNeedMore);
  uint8_t foreign[20];
  memcpy(foreign, b, lb);
  foreign[2] ^= 1U;
  assert(r.push(foreign, lb, 0) == el::Reassembler::Result::kDropped);
  uint8_t badver[20];
  memcpy(badver, a, la);
  badver[0] = 9;
  assert(r.push(badver, la, 0) == el::Reassembler::Result::kDropped);
  // Stale partial expires.
  assert(r.push(a, la, 100) == el::Reassembler::Result::kNeedMore);
  r.poll(100 + el::kReassemblyTimeoutMs);
  assert(r.push(b, lb, 100 + el::kReassemblyTimeoutMs) ==
         el::Reassembler::Result::kDropped);
  assert(r.push(a, la, 0) == el::Reassembler::Result::kNeedMore);
  assert(r.push(b, lb, 0) == el::Reassembler::Result::kNeedMore);
  assert(r.push(c, lc, 0) == el::Reassembler::Result::kComplete);
  assert(r.size() == 40U && memcmp(r.message(), msg, 40) == 0);
}

uint16_t get16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint64_t get64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

struct Sent { uint8_t type; std::vector<uint8_t> bytes; };

Sent pull(Sf5l0EdgeLinkSession& s) {
  uint8_t out[el::kMaxMessage];
  size_t n = 0;
  if (!s.next(out, sizeof(out), n)) return Sent{0, {}};
  // Every message survives the 20-byte framing unchanged.
  el::Fragmenter f;
  el::Reassembler r;
  assert(f.load(out, n, 1));
  while (f.pending()) {
    uint8_t frame[el::kFrameSize];
    uint8_t len = 0;
    assert(f.peek(frame, len));
    r.push(frame, len, 0);
    f.advance();
  }
  assert(r.size() == n && memcmp(r.message(), out, n) == 0);
  return Sent{out[0], std::vector<uint8_t>(out, out + n)};
}

std::vector<uint8_t> acceptMessage(uint16_t page, uint16_t slot, uint64_t gen,
                                   const uint8_t* object, size_t size,
                                   bool corrupt = false) {
  el::Accept a{};
  a.page = page;
  a.slot = slot;
  a.generation = gen;
  assert(Sf5l0EdgeHandoff::mac(kGateway, page, slot, gen, object, size, a.mac));
  if (corrupt) a.mac[0] ^= 1U;
  uint8_t out[el::kAcceptSize];
  assert(el::encodeAccept(a, out, sizeof(out)) == el::kAcceptSize);
  return std::vector<uint8_t>(out, out + sizeof(out));
}

// Glue the gateway does: handoff events go to the session that asked.
void settle(Gateway& gw, Sf5l0EdgeLinkSession& s) {
  for (unsigned i = 0; i < 50U; ++i) {
    const auto e = gw.handoff.service();
    if (e.kind == Sf5l0EdgeHandoff::Event::Kind::kHandedOff)
      s.onAcceptDone(e.page, e.slot, e.ok);
    else if (e.kind == Sf5l0EdgeHandoff::Event::Kind::kReclaimed)
      s.onReclaimed(e.page, e.ok);
  }
}

void testSession() {
  EraseFlash flash;
  Gateway gw(flash);
  for (uint64_t c = 1U; c <= 40U; ++c) {
    osc::ExactObject o{};
    makeFrame(o, kTracker, sf5l0_bench::kTestKeyEpoch, c);
    assert(gw.receive(o));
  }
  Sf5l0EdgeLinkSession s(flash, gw.store, gw.handoff, kGateway, kPages);
  assert(pull(s).type == 0);  // nothing before a connection
  s.begin();

  Sent hello = pull(s);
  assert(hello.type == uint8_t(el::Type::kHello) &&
         hello.bytes.size() == el::kHelloSize);
  assert(get64(&hello.bytes[1]) == kGateway && get16(&hello.bytes[9]) == 40U &&
         get16(&hello.bytes[11]) == kPages * fmt::kRecordsPerPage);

  // Full pass: every held record once, oldest first, then DUMP_END.
  std::vector<Sent> sent_held;
  for (;;) {
    Sent m = pull(s);
    if (m.type == uint8_t(el::Type::kDumpEnd)) {
      assert(get16(&m.bytes[1]) == 40U);
      break;
    }
    assert(m.type == uint8_t(el::Type::kHeld));
    assert(m.bytes.size() == el::kHeldHeader + 100U && m.bytes[17] == 100U);
    assert((m.bytes[13] & el::kHeldFlagLink) == 0U);
    sent_held.push_back(m);
  }
  assert(sent_held.size() == 40U);
  for (size_t i = 1; i < sent_held.size(); ++i) {
    const uint64_t g0 = get64(&sent_held[i - 1].bytes[5]), g1 = get64(&sent_held[i].bytes[5]);
    assert(g0 < g1 || (g0 == g1 && get16(&sent_held[i - 1].bytes[3]) <
                                       get16(&sent_held[i].bytes[3])));
  }
  assert(pull(s).type == 0);  // caught up: idle, no rescans

  auto field = [](const Sent& m, uint16_t& page, uint16_t& slot, uint64_t& gen,
                  const uint8_t*& obj) {
    page = get16(&m.bytes[1]);
    slot = get16(&m.bytes[3]);
    gen = get64(&m.bytes[5]);
    obj = &m.bytes[el::kHeldHeader];
  };

  // A wrong MAC and a malformed message are answered, never acted on.
  {
    uint16_t p, sl; uint64_t g; const uint8_t* o;
    field(sent_held[0], p, sl, g, o);
    const auto bad = acceptMessage(p, sl, g, o, 100, true);
    s.onMessage(bad.data(), bad.size());
    Sent r = pull(s);
    assert(r.type == uint8_t(el::Type::kAcceptResult) &&
           r.bytes[5] == uint8_t(el::AcceptCode::kBadMac));
    const uint8_t junk[] = {uint8_t(el::Type::kAccept), 1, 2};
    s.onMessage(junk, sizeof(junk));
    r = pull(s);
    assert(r.type == uint8_t(el::Type::kAcceptResult) &&
           r.bytes[5] == uint8_t(el::AcceptCode::kMalformed));
  }

  // Accept the first 33 (the full first page): each answered HANDED_OFF,
  // then the page is reclaimed and the Edge is told.
  bool reclaimed = false;
  for (size_t i = 0; i < 33U; ++i) {
    uint16_t p, sl; uint64_t g; const uint8_t* o;
    field(sent_held[i], p, sl, g, o);
    const auto ok = acceptMessage(p, sl, g, o, 100);
    s.onMessage(ok.data(), ok.size());
    assert(s.acceptPending());
    settle(gw, s);
    for (Sent r = pull(s); r.type != 0; r = pull(s)) {
      if (r.type == uint8_t(el::Type::kAcceptResult)) {
        assert(get16(&r.bytes[1]) == p && get16(&r.bytes[3]) == sl &&
               r.bytes[5] == uint8_t(el::AcceptCode::kHandedOff));
      } else {
        assert(r.type == uint8_t(el::Type::kReclaimed) && r.bytes[3] == 1U);
        reclaimed = true;
      }
    }
    settle(gw, s);
    for (Sent r = pull(s); r.type != 0; r = pull(s)) {
      assert(r.type == uint8_t(el::Type::kReclaimed) && r.bytes[3] == 1U);
      reclaimed = true;
    }
  }
  assert(reclaimed && gw.handoff.reclaimed() == 1U);
  // Repeat of an accepted record after its page was reclaimed: UNKNOWN.
  {
    uint16_t p, sl; uint64_t g; const uint8_t* o;
    field(sent_held[0], p, sl, g, o);
    const auto again = acceptMessage(p, sl, g, o, 100);
    s.onMessage(again.data(), again.size());
    const Sent r = pull(s);
    assert(r.type == uint8_t(el::Type::kAcceptResult) &&
           r.bytes[5] == uint8_t(el::AcceptCode::kUnknown));
  }

  // DUMP: a new full pass shows only what is still held (7).
  const uint8_t dump[] = {uint8_t(el::Type::kDump)};
  s.onMessage(dump, sizeof(dump));
  unsigned again = 0;
  for (Sent m = pull(s); m.type != uint8_t(el::Type::kDumpEnd); m = pull(s)) {
    assert(m.type == uint8_t(el::Type::kHeld));
    ++again;
  }
  assert(again == 7U);

  // A live record streams at once, with this Gateway's RX link facts.
  osc::ExactObject live{};
  makeFrame(live, kTracker, sf5l0_bench::kTestKeyEpoch, 41U);
  assert(gw.receive(live));
  const auto h = held(flash, gw.store);
  const auto& newest = h.back();
  s.onNewRecord(newest.page, newest.slot, newest.generation, true, -97, 7);
  Sent m = pull(s);
  assert(m.type == uint8_t(el::Type::kHeld) &&
         (m.bytes[13] & el::kHeldFlagLink) != 0U &&
         int16_t(get16(&m.bytes[14])) == -97 && int8_t(m.bytes[16]) == 7 &&
         memcmp(&m.bytes[el::kHeldHeader], live.object, 100) == 0);
  assert(pull(s).type == 0);

  // A disconnected session sends nothing; a new one starts with HELLO again.
  s.end();
  assert(pull(s).type == 0);
  s.begin();
  assert(pull(s).type == uint8_t(el::Type::kHello));
}

}  // namespace

int main() {
  testMacVector();          // shared with SF5L5: same accept rule over BLE
  testHandoffAndReclaim();
  testFraming();
  testSession();
  puts("SF5L6 BLE Edge link: framing/reassembly, HELLO/HELD/DUMP_END pass, "
       "accept results, reclaim notice, live record with RX link: PASS "
       "(HOST ONLY)");
  return 0;
}
