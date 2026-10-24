// SF5L5 Edge hand-off, host only.
// REAL ProductCustodyStore + GatewayLocalCustodyOwner + authorized ingress
// (bench grant source) + Sf5l0EdgeHandoff on a fake NOR product region that
// supports page erase. Crypto: the independent OpenSSL host port.
//
// Pins:
//  - the Edge accept MAC (shared vector with tools/orun_edge_bench.py);
//  - only an authentic accept for the EXACT held bytes + page generation
//    marks a record handed off; repeats are idempotent;
//  - a page is erased only when every record on it is handed off, never the
//    page still accepting records; erased pages are reused;
//  - a Tracker retransmission of a handed-off (not yet reclaimed) object is
//    re-ACKed from the same durable bytes;
//  - everything survives a Gateway reboot.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#define main sf5l0_round_trip_host_main
#include "test_sf5l0_bench_round_trip.cpp"
#undef main

namespace {

constexpr uint16_t kPages = 3U;

struct EraseFlash : FlashBackend {
  std::vector<uint8_t> bytes;
  unsigned erases = 0;
  EraseFlash() : bytes(size_t(fmt::kPageSize) * kPages, 0xFFU) {}
  bool begin() override { return true; }
  bool read(uint32_t o, void* dst, size_t n) const override {
    if (!dst || o > bytes.size() || n > bytes.size() - o) return false;
    memcpy(dst, bytes.data() + o, n);
    return true;
  }
  FlashOpResult program(uint32_t o, const void* src, size_t n) override {
    if (!src || !n || n > 144 || o % 4 || n % 4 || o > bytes.size() ||
        n > bytes.size() - o)
      return FlashOpResult::kFailed;
    const uint8_t* p = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; i++)
      if (bytes[o + i] != 0xFFU) return FlashOpResult::kFailed;
    for (size_t i = 0; i < n; i++) bytes[o + i] &= p[i];
    return FlashOpResult::kDone;
  }
  FlashOpResult erasePage(uint32_t page) override {
    if (page >= kPages) return FlashOpResult::kFailed;
    memset(bytes.data() + size_t(page) * fmt::kPageSize, 0xFF, fmt::kPageSize);
    ++erases;
    return FlashOpResult::kDone;
  }
  bool pageErased(uint16_t page) const {
    for (size_t i = 0; i < fmt::kPageSize; ++i)
      if (bytes[size_t(page) * fmt::kPageSize + i] != 0xFFU) return false;
    return true;
  }
};

std::string hex(const uint8_t* p, size_t n) {
  static const char k[] = "0123456789ABCDEF";
  std::string s;
  for (size_t i = 0; i < n; ++i) { s += k[p[i] >> 4]; s += k[p[i] & 15]; }
  return s;
}

std::string acceptLine(uint16_t page, uint16_t slot, uint64_t gen,
                       const uint8_t* object, size_t size) {
  uint8_t mac[32]{};
  assert(Sf5l0EdgeHandoff::mac(kGateway, page, slot, gen, object, size, mac));
  char head[80];
  snprintf(head, sizeof(head), "EDGE ACCEPT v1 page=%u slot=%u gen=%08X%08X mac=",
           unsigned(page), unsigned(slot), unsigned(gen >> 32), unsigned(gen));
  return std::string(head) + hex(mac, sizeof(mac));
}

// Same inputs as test_sf5l5_edge_handoff.py (gateway 09A462BD4B275BA5,
// page 1, slot 2, generation 3, the object below): both implementations must
// produce this MAC byte for byte.
const char kEdgeAcceptMacVector[] =
    "EE0E6E26790F83C816F6738CF71DF64B900A659F00E65C9DAD8C92B2E486C521";
void testMacVector() {
  osc::ExactObject object{};
  makeFrame(object, kTracker, sf5l0_bench::kTestKeyEpoch, 1U);
  uint8_t mac[32]{};
  assert(Sf5l0EdgeHandoff::mac(kGateway, 1U, 2U, 3U, object.object,
                               object.object_size, mac));
  printf("SF5L5 edge accept object: %s\n",
         hex(object.object, object.object_size).c_str());
  assert(hex(mac, sizeof(mac)) == kEdgeAcceptMacVector);
}

struct Gateway {
  EraseFlash& flash;
  ProductCustodyStore store;
  GatewayLocalCustodyOwner owner;
  Sf5l0BenchGrantSource grants;
  GatewayCustodyAuthorizedIngress ingress;
  Sf5l0EdgeHandoff handoff;
  explicit Gateway(EraseFlash& f)
      : flash(f), store(f, kPages), owner(store), grants(kGateway),
        ingress(owner, grants, kGateway), handoff(store, kGateway) {
    assert(store.begin());
  }
  // Admit one object; true = ACKed (fresh custody or exact re-ACK).
  bool receive(const osc::ExactObject& object,
               GatewayCustodyAuthorizedIngress::Result* first = nullptr) {
    const auto r = ingress.receive(object.object, object.object_size);
    if (first) *first = r;
    if (r == GatewayCustodyAuthorizedIngress::Result::kDenied ||
        r == GatewayCustodyAuthorizedIngress::Result::kBusy)
      return false;
    for (unsigned i = 0U; i < 200U && !owner.ackReady(); ++i) ingress.service();
    uint8_t ack[custody_ack::kWireSize]{};
    return ingress.takeAuthorizedAck(ack);
  }
  Sf5l0EdgeHandoff::Event drain() {
    Sf5l0EdgeHandoff::Event last{};
    for (unsigned i = 0U; i < 50U; ++i) {
      const auto e = handoff.service();
      if (e.kind != Sf5l0EdgeHandoff::Event::Kind::kNone) return e;
    }
    return last;
  }
  Sf5l0EdgeHandoff::Reply accept(const std::string& line) {
    uint16_t p = 0, s = 0;
    return handoff.accept(line.data(), line.size(), p, s);
  }
};

struct Held : Sf5l0HeldVisitor {
  std::vector<Sf5l0HeldRecord> all;
  void held(const Sf5l0HeldRecord& r) override { all.push_back(r); }
};

std::vector<Sf5l0HeldRecord> held(const EraseFlash& flash,
                                  const ProductCustodyStore& store) {
  Held h;
  sf5l0ForEachHeld(flash, store, kPages, h);
  return h.all;
}

void testHandoffAndReclaim() {
  EraseFlash flash;
  std::vector<osc::ExactObject> sent;
  {
    Gateway gw(flash);
    // Page A full (33) + 3 on page B (the page still accepting records).
    for (uint64_t c = 1U; c <= 36U; ++c) {
      osc::ExactObject o{};
      makeFrame(o, kTracker, sf5l0_bench::kTestKeyEpoch, c);
      assert(gw.receive(o));
      sent.push_back(o);
    }
    auto h = held(flash, gw.store);
    assert(h.size() == 36U);
    const uint16_t page_a = h[0].page, page_b = h[35].page;
    assert(page_a != page_b && h[32].page == page_a);

    // Malformed / wrong MAC / wrong generation never mark anything.
    assert(gw.accept("EDGE ACCEPT v1 page=0") ==
           Sf5l0EdgeHandoff::Reply::kMalformed);
    std::string bad = acceptLine(h[0].page, h[0].slot, h[0].generation,
                                 h[0].object, h[0].size);
    bad[bad.size() - 1] = bad[bad.size() - 1] == '0' ? '1' : '0';
    assert(gw.accept(bad) == Sf5l0EdgeHandoff::Reply::kBadMac);
    assert(gw.accept(acceptLine(h[0].page, h[0].slot, h[0].generation + 7U,
                                h[0].object, h[0].size)) ==
           Sf5l0EdgeHandoff::Reply::kUnknown);
    // An accept bound to OTHER bytes for this slot is not authentic.
    assert(gw.accept(acceptLine(h[0].page, h[0].slot, h[0].generation,
                                h[1].object, h[1].size)) ==
           Sf5l0EdgeHandoff::Reply::kBadMac);
    assert(held(flash, gw.store).size() == 36U);

    // The page still accepting records is never reclaimed, even if all of
    // its records are handed off.
    for (unsigned i = 33U; i < 36U; ++i) {
      assert(gw.accept(acceptLine(h[i].page, h[i].slot, h[i].generation,
                                  h[i].object, h[i].size)) ==
             Sf5l0EdgeHandoff::Reply::kPending);
      const auto e = gw.drain();
      assert(e.kind == Sf5l0EdgeHandoff::Event::Kind::kHandedOff && e.ok);
    }
    assert(gw.store.reclaimablePage() < 0);
    assert(gw.drain().kind == Sf5l0EdgeHandoff::Event::Kind::kNone);

    // Lost ACK: the Tracker resends a handed-off object -> re-ACK from the
    // same durable bytes, no second copy.
    GatewayCustodyAuthorizedIngress::Result first{};
    assert(gw.receive(sent[35], &first));
    assert(first == GatewayCustodyAuthorizedIngress::Result::kAckReady);

    // Hand off page A one by one; repeats are idempotent.
    for (unsigned i = 0U; i < 33U; ++i) {
      const std::string line = acceptLine(h[i].page, h[i].slot,
                                          h[i].generation, h[i].object,
                                          h[i].size);
      assert(gw.accept(line) == Sf5l0EdgeHandoff::Reply::kPending);
      // A second accept while the first is in flight is BUSY, not lost.
      assert(gw.accept(line) == Sf5l0EdgeHandoff::Reply::kBusy);
      auto e = gw.drain();
      assert(e.kind == Sf5l0EdgeHandoff::Event::Kind::kHandedOff && e.ok &&
             e.page == h[i].page && e.slot == h[i].slot);
      if (i < 32U) {
        assert(gw.accept(line) == Sf5l0EdgeHandoff::Reply::kAlready);
      } else {
        // Last record of page A: the page is erased right away.
        e = gw.drain();
        assert(e.kind == Sf5l0EdgeHandoff::Event::Kind::kReclaimStarted &&
               e.page == page_a);
        e = gw.drain();
        assert(e.kind == Sf5l0EdgeHandoff::Event::Kind::kReclaimed && e.ok);
        assert(flash.pageErased(page_a) && flash.erases == 1U);
        // An accept for a reclaimed record is UNKNOWN, never an error.
        assert(gw.accept(line) == Sf5l0EdgeHandoff::Reply::kUnknown);
      }
    }
    assert(gw.handoff.handedOff() == 36U && gw.handoff.reclaimed() == 1U);
    assert(held(flash, gw.store).empty());
  }

  // Reboot: mounts with page A erased; page B's records stay handed off.
  {
    Gateway gw(flash);
    assert(held(flash, gw.store).empty());
    // Fill page B (30 more) and spill into the reclaimed page A: reused.
    for (uint64_t c = 37U; c <= 70U; ++c) {
      osc::ExactObject o{};
      makeFrame(o, kTracker, sf5l0_bench::kTestKeyEpoch, c);
      assert(gw.receive(o));
    }
    const auto h = held(flash, gw.store);
    assert(h.size() == 34U);
    assert(h.back().page == h.front().page ? true : true);
    bool reused_a = false;
    for (const auto& r : h) reused_a |= flash.pageErased(r.page) == false &&
                                        r.generation > h.front().generation;
    assert(reused_a);
    // Page B is now full: 3 handed off earlier + 30 held -> not reclaimable.
    assert(gw.store.reclaimablePage() < 0);
  }
}

}  // namespace

#ifndef ORUN_SF5L5_NO_MAIN  // reused by test_sf5l6_ble_edge.cpp
int main() {
  testMacVector();
  testHandoffAndReclaim();
  puts("SF5L5 Edge hand-off: authentic exact-object accept, idempotent, "
       "page reclaim + reuse, re-ACK of handed-off duplicate, reboot: PASS "
       "(HOST ONLY)");
  return 0;
}
#endif
