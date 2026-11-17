#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <array>

#include "custody_ack_primitives.h"
#include "gateway_grant_snapshot_reader.h"
#include "kq05_gateway_signed_link_credentials.h"
#include "kq05_gateway_ram_relay.h"
#include "kq05_edge_link_wire.h"
#include "observation_store_format.h"

#ifndef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE
#error "KQ05 experimental lab only"
#endif

namespace fmt = orun_tlp::gateway_grant_format;
namespace kr = orun_tlp::kq05_ram_relay;
namespace wire = orun_tlp::kq05_edge_link;
using orun_tlp::GatewayGrantSnapshotReader;
using orun_tlp::GatewayEnrollmentProofVerifier;
using orun_tlp::FlashBackend;
using orun_tlp::FlashOpResult;
using orun_tlp::custody_crypto::hmacSha256Bounded;

namespace {
constexpr uint64_t kGateway = UINT64_C(0x4741544557415931);
constexpr uint64_t kTracker = UINT64_C(0x545241434b455231);
constexpr char kEdge[] = "edge-one";
constexpr char kDerive[] = "ORUN-KQ05-OGK1-EDGE-LINK-DERIVE-v1";
constexpr char kOfferDomain[] = "ORUN-KQ05-EDGE-GATEWAY-OFFER-v1";

struct FakeNor final : FlashBackend {
  std::array<uint8_t, fmt::kPageBytes * fmt::kPageCount> bytes{};
  bool fault = false;
  unsigned writes = 0;
  FakeNor() { bytes.fill(0xFFU); }
  bool begin() override { return true; }
  bool read(uint32_t offset, void* out, size_t count) const override {
    if (out == nullptr || fault || offset > bytes.size() ||
        count > bytes.size() - offset) return false;
    memcpy(out, bytes.data() + offset, count);
    return true;
  }
  bool hasUnreconciledMutation() const override { return fault; }
  FlashOpResult program(uint32_t, const void*, size_t) override {
    ++writes; return FlashOpResult::kFailed;
  }
  FlashOpResult erasePage(uint32_t) override {
    ++writes; return FlashOpResult::kFailed;
  }
};

void fakeProof(const uint8_t* data, size_t n, uint8_t (&tag)[64]) {
  const uint32_t crc = orun_tlp::observation_store_format::crc32(data, n);
  for (size_t i=0; i<64U; ++i)
    tag[i] = static_cast<uint8_t>((crc >> ((i%4U)*8U)) ^ (i*23U+0x5bU));
}

struct FakeIssuer final : GatewayEnrollmentProofVerifier {
  bool verify(uint8_t version, const uint8_t* data, size_t n,
              const uint8_t (&proof)[fmt::kProofBytes]) const override {
    if (version != fmt::kEdgeKeyVersion || n != fmt::kEdgePayloadBytes)
      return false;
    uint8_t expected[64]{};
    fakeProof(data, n, expected);
    return memcmp(expected, proof, 64U) == 0;
  }
};

void installFakeSnapshot(FakeNor& flash, bool v2=true) {
  uint8_t* p=flash.bytes.data();
  const uint8_t version = v2 ? fmt::kEdgeKeyVersion : fmt::kVersion;
  const size_t payload = fmt::payloadBytes(version);
  const size_t stored = fmt::storedBytes(version);
  memset(p, 0U, stored);
  orun_tlp::observation_store_format::put32(p, fmt::kMagic);
  p[4] = version; p[5] = static_cast<uint8_t>(fmt::kGrantSlots);
  orun_tlp::observation_store_format::put64(p+8U, kGateway);
  orun_tlp::observation_store_format::put64(p+16U, 1U);
  orun_tlp::observation_store_format::put32(p+24U, 1U);
  uint8_t* grant=p+fmt::kHeaderBytes;
  orun_tlp::observation_store_format::put64(grant,kTracker);
  orun_tlp::observation_store_format::put32(grant+8U,7U);
  orun_tlp::observation_store_format::put32(grant+12U,10U);
  orun_tlp::observation_store_format::put32(grant+16U,3U);
  orun_tlp::observation_store_format::put32(grant+20U,1U);
  for(unsigned i=0U;i<32U;++i)
    grant[24U+i]=static_cast<uint8_t>(0x20U+i);
  if (v2) for(unsigned i=0U;i<32U;++i)
    p[fmt::kPayloadBytes+i]=static_cast<uint8_t>(0x40U+i);
  uint8_t proof[64]{};
  fakeProof(p,payload,proof);
  memcpy(p+payload,proof,64U);
  orun_tlp::observation_store_format::put32(
      p+payload+64U,orun_tlp::observation_store_format::crc32(p,payload+64U));
  orun_tlp::observation_store_format::put32(p+payload+68U,0U);
}

void signedOffer(const uint8_t (&link_key)[32], uint8_t (&frame)[160],
                 size_t& size, uint32_t epoch=7U) {
  uint8_t raw[95]{};
  raw[0]=1U;raw[1]=0x91U;
  orun_tlp::observation_store_format::put64(raw+2U,kGateway);
  orun_tlp::observation_store_format::put64(raw+10U,1U);
  raw[18U]=44U;
  uint8_t* receipt=raw+19U;
  receipt[0]=2U;receipt[1]=9U;receipt[3]=1U;
  orun_tlp::observation_store_format::put64(receipt+4U,kTracker);
  orun_tlp::observation_store_format::put64(
      receipt+12U,UINT64_C(0x494e4341524e3031));
  orun_tlp::observation_store_format::put32(receipt+20U,epoch);
  orun_tlp::observation_store_format::put32(receipt+24U,101U);
  for(size_t i=0U;i<16U;++i)receipt[28U+i]=static_cast<uint8_t>(0x30U+i);

  uint8_t input[140]{};
  memcpy(input,kOfferDomain,sizeof(kOfferDomain));
  memcpy(input+sizeof(kOfferDomain),kEdge,sizeof(kEdge));
  memcpy(input+sizeof(kOfferDomain)+sizeof(kEdge),raw,63U);
  uint8_t tag[32]{};
  assert(hmacSha256Bounded(link_key,input,
         sizeof(kOfferDomain)+sizeof(kEdge)+63U,tag));
  memcpy(raw+63U,tag,32U);
  size=wire::wrapOffer(raw,95U,frame,160U);
  assert(size==96U);
}

void testSignedV2ScopeAndDerivation() {
  FakeNor nor;
  installFakeSnapshot(nor);
  FakeIssuer issuer;
  GatewayGrantSnapshotReader reader(nor,issuer);
  assert(reader.begin(kGateway,true,false)==
         GatewayGrantSnapshotReader::Status::kReady);
  kr::SignedGrantCredentials auth(reader,kGateway,kEdge);
  assert(auth.verifiedForGateway(kGateway));
  assert(!auth.verifiedForGateway(kGateway+1U));
  assert(auth.permitsTracker(kGateway,kTracker,7U));
  assert(!auth.permitsTracker(kGateway,kTracker,8U));
  assert(!auth.permitsTracker(kGateway,kTracker+1U,7U));

  char edge[65]{};
  uint8_t derived[32]{};
  assert(auth.readGatewayLink(kGateway,edge,derived));
  assert(strcmp(edge,kEdge)==0);
  uint8_t master[32]{};
  for(unsigned i=0U;i<32U;++i)
    master[i]=static_cast<uint8_t>(0x40U+i);
  uint8_t data[130]{};
  memcpy(data,kDerive,sizeof(kDerive));
  orun_tlp::observation_store_format::put64(data+sizeof(kDerive),kGateway);
  memcpy(data+sizeof(kDerive)+8U,kEdge,sizeof(kEdge)-1U);
  uint8_t expected[32]{};
  assert(hmacSha256Bounded(
      master,data,sizeof(kDerive)+8U+sizeof(kEdge)-1U,expected));
  assert(memcmp(expected,derived,32U)==0);
  const char pinned[] =
      "cfa987ef239b763e3dc1b05d3f7bd3bda8c675659ab0c1935d386e1e466ba84f";
  char actual[65]{};
  for (size_t i=0U; i<32U; ++i)
    snprintf(actual+2U*i,sizeof(actual)-2U*i,"%02x",unsigned(derived[i]));
  assert(strcmp(actual,pinned)==0); // shared Edge Python test vector
  assert(memcmp(master,derived,32U)!=0); // purpose-separated key

  kr::GatewayRamRelay q(kGateway);
  uint8_t frame[160]{};
  size_t n=0U;
  signedOffer(derived,frame,n);
  assert(q.accept(frame,n,100U,true,auth)==kr::Admission::kQueued);
  signedOffer(derived,frame,n,8U);
  assert(q.accept(frame,n,101U,true,auth)==kr::Admission::kDenied);
  assert(q.pending(102U)==1U);
  reader.denyForCurrentBoot();
  assert(!auth.verifiedForGateway(kGateway));
  assert(!auth.readGatewayLink(kGateway,edge,derived));
  assert(q.accept(frame,n,103U,true,auth)==kr::Admission::kDenied);
  assert(q.pending(103U)==0U);
  assert(nor.writes==0U);
}

void testBadEnrollmentNeverOpens() {
  FakeNor blank;
  FakeIssuer issuer;
  GatewayGrantSnapshotReader absent(blank,issuer);
  assert(absent.begin(kGateway,true,false)==
         GatewayGrantSnapshotReader::Status::kEmpty);
  kr::SignedGrantCredentials bad(absent,kGateway,kEdge);
  assert(!bad.verifiedForGateway(kGateway));

  FakeNor nor;
  installFakeSnapshot(nor);
  GatewayGrantSnapshotReader reader(nor,issuer);
  assert(reader.begin(kGateway,true,false)==
         GatewayGrantSnapshotReader::Status::kReady);
  kr::SignedGrantCredentials bad_name(reader,kGateway,"malformed!");
  assert(!bad_name.verifiedForGateway(kGateway));
  kr::SignedGrantCredentials bad_gateway(reader,kGateway+1U,kEdge);
  assert(!bad_gateway.verifiedForGateway(kGateway+1U));
  nor.fault=true;
  kr::SignedGrantCredentials rev(reader,kGateway,kEdge);
  assert(!rev.verifiedForGateway(kGateway));
}
} // namespace

int main() {
  testSignedV2ScopeAndDerivation();
  testBadEnrollmentNeverOpens();
  puts("KQ05 signed OGK1 v2 link key/epoch/deny and RAM offer: PASS (FAKE ISSUER HOST ONLY)");
  return 0;
}
