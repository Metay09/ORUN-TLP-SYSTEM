#if defined(ORUN_GATEWAY_BATCH_ACK) || defined(ORUN_TRACKER_BATCH_ACK)
#include "kq05_tracker_batch_ack_verifier.h"

#include <string.h>

#include "tlp_v2_product_secure.h"
#include "custody_ack_primitives.h"
#include "kq05_gateway_batch_custody_crypto.h"
#include "product_record_store.h"
#include "security_store.h"

namespace orun_tlp {
namespace {
namespace ack = kq05_gateway_batch_ack;
// osc/osf aliases are already declared at orun_tlp scope by
// observation_store.h. Do not redeclare them in the anonymous namespace:
// GCC 7 treats the two visible aliases as ambiguous.

constexpr char kKeyInfo[] = "ORUN-TLP-V2-GW-CUSTODY-ACK-v1";
constexpr size_t kInfoSize = sizeof(kKeyInfo)-1U + 8U + 4U + 8U + 4U + 4U;
static_assert(kInfoSize == 57U, "Custody grant HKDF scope changed");

void wipe(void* pointer, size_t count) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(pointer);
  while (count-- != 0U) *p++ = 0U;
}
bool equalHint(const uint8_t* a, const uint8_t* b) {
  volatile uint8_t difference=0U;
  for (size_t i=0U; i<ack::kRefSize; ++i)
    difference=static_cast<uint8_t>(difference | (a[i]^b[i]));
  return difference==0U;
}
void put32(uint32_t v, uint8_t* p) {
  p[0]=static_cast<uint8_t>(v>>24U);
  p[1]=static_cast<uint8_t>(v>>16U);
  p[2]=static_cast<uint8_t>(v>>8U);
  p[3]=static_cast<uint8_t>(v);
}
void put64(uint64_t v, uint8_t* p) {
  for (size_t i=0U;i<8U;++i)
    p[i]=static_cast<uint8_t>(v>>(56U-8U*i));
}
void deriveInfo(const ack::Header& header, uint8_t (&info)[kInfoSize]) {
  size_t off=0U;
  memcpy(info,kKeyInfo,sizeof(kKeyInfo)-1U);
  off+=sizeof(kKeyInfo)-1U;
  put64(header.tracker_device_id,info+off);off+=8U;
  put32(header.key_epoch,info+off);off+=4U;
  put64(header.gateway_device_id,info+off);off+=8U;
  put32(header.custody_floor,info+off);off+=4U;
  put32(header.grant_generation,info+off);
}

bool eligibleObject(const ack::Header& batch,
                    const osc::ExactObject& object) {
  if (!object.identity.valid() || object.object_size > sizeof(object.object) ||
      object.object_size == 0U ||
      (object.record_kind != osf::RecordKind::kPeriodic &&
       object.record_kind != osf::RecordKind::kEvent)) return false;
  return tlp::matchesCustodyObjectShape(
      batch.tracker_device_id, batch.key_epoch,
      object.object, object.object_size);
}
} // namespace

Kq05TrackerBatchVerifyResult Kq05TrackerBatchAckVerifier::verifyFromStore(
    const uint8_t* wire, size_t length, const ProductRecordStore& store,
    AuthenticatedKq05TrackerBatchAck& output) {
  ack::Header header{};
  if (!ack::decode(wire,length,header))
    return Kq05TrackerBatchVerifyResult::kInvalid;
  if (!security_.custodyAckScopeMatches(
          header.tracker_device_id,header.gateway_device_id,
          header.key_epoch,header.custody_floor,header.grant_generation))
    return Kq05TrackerBatchVerifyResult::kNoDurableGrant;

  osc::ExactObject cached[osf::kMaxExactCustodyObjects]{};
  uint16_t cached_count=0U;
  if (!store.snapshotCustodyCandidates(cached,cached_count))
    return Kq05TrackerBatchVerifyResult::kStoreUnavailable;
  if (cached_count < header.count)
    return Kq05TrackerBatchVerifyResult::kNoUniqueObject;

  uint8_t digests[osf::kMaxExactCustodyObjects][32]{};
  bool valid[osf::kMaxExactCustodyObjects]{};
  for (uint16_t i=0U;i<cached_count;++i) {
    if (!eligibleObject(header,cached[i])) continue;
    if (!custody_crypto::sha256(
            cached[i].object,cached[i].object_size,digests[i])) {
      wipe(digests,sizeof(digests));
      return Kq05TrackerBatchVerifyResult::kCryptoFailure;
    }
    valid[i]=true;
  }

  Kq05BatchExactView selected[ack::kMaxObjects]{};
  AuthenticatedKq05TrackerBatchAck proven{};
  proven.tracker_id_=header.tracker_device_id;
  proven.gateway_id_=header.gateway_device_id;
  proven.key_epoch_=header.key_epoch;
  proven.floor_=header.custody_floor;
  proven.generation_=header.grant_generation;
  proven.count_=header.count;
  uint8_t used[osf::kMaxExactCustodyObjects]{};

  for (uint8_t i=0U;i<header.count;++i) {
    uint16_t matched=0U;
    uint16_t selected_index=UINT16_MAX;
    for (uint16_t j=0U;j<cached_count;++j) {
      if (!valid[j] || !equalHint(header.refs[i],digests[j])) continue;
      ++matched;
      selected_index=j;
    }
    if (matched != 1U || selected_index==UINT16_MAX ||
        used[selected_index]!=0U) {
      wipe(digests,sizeof(digests));
      return Kq05TrackerBatchVerifyResult::kNoUniqueObject;
    }
    used[selected_index]=1U;
    if (!store.custodyCandidateMatchesRetainedExact(cached[selected_index])) {
      wipe(digests,sizeof(digests));
      return Kq05TrackerBatchVerifyResult::kStoreUnavailable;
    }
    selected[i].bytes=cached[selected_index].object;
    selected[i].size=cached[selected_index].object_size;
    proven.identities_[i]=cached[selected_index].identity;
    memcpy(proven.full_digests_[i],digests[selected_index],32U);
  }

  // The candidate reference is only an 8-byte INDEX; the full digest for
  // EVERY matched exact frame must be protected by the HMAC. The key comes
  // exclusively from the actual, durable Tracker SecurityStore V3 grant.
  GatewayCustodyAckScope scope{};
  scope.tracker_device_id=header.tracker_device_id;
  scope.gateway_device_id=header.gateway_device_id;
  scope.key_epoch=header.key_epoch;
  scope.custody_floor=header.custody_floor;
  scope.grant_generation=header.grant_generation;
  uint8_t salt[16]{},root[32]{},key[32]{},info[kInfoSize]{};
  memcpy(salt,security_.credential_.credential_id,sizeof(salt));
  memcpy(root,security_.credential_.k_root,sizeof(root));
  deriveInfo(header,info);
  const bool derived=custody_crypto::hkdfSha256(
      salt,root,info,sizeof(info),key);
  wipe(root,sizeof(root));
  wipe(salt,sizeof(salt));
  wipe(info,sizeof(info));

  const bool authentic=derived &&
      Kq05GatewayBatchCustodyCrypto::verifyAgainstExactRetained(
          wire,length,scope,key,selected,header.count);
  wipe(key,sizeof(key));
  wipe(digests,sizeof(digests));
  if (!derived) return Kq05TrackerBatchVerifyResult::kCryptoFailure;
  if (!authentic) return Kq05TrackerBatchVerifyResult::kBadMac;

  // Scope may be revoked during CC310 execution. Also independently
  // reread each claimed record before publishing any release capability.
  if (!security_.custodyAckScopeMatches(
          header.tracker_device_id,header.gateway_device_id,
          header.key_epoch,header.custody_floor,header.grant_generation))
    return Kq05TrackerBatchVerifyResult::kNoDurableGrant;

  for (uint8_t i=0U;i<header.count;++i) {
    osc::ExactObject actual{};
    if (store.findCachedExact(proven.identities_[i],actual) !=
            ObservationStore::ControlLookupResult::kFound ||
        actual.object_size == 0U || actual.object_size > sizeof(actual.object) ||
        !store.custodyCandidateMatchesRetainedExact(actual))
      return Kq05TrackerBatchVerifyResult::kStoreUnavailable;
    uint8_t digest[32]{};
    const bool got=custody_crypto::sha256(
        actual.object,actual.object_size,digest);
    volatile uint8_t diff=0U;
    for (size_t j=0U;j<sizeof(digest);++j)
      diff=static_cast<uint8_t>(diff|
          (digest[j]^proven.full_digests_[i][j]));
    wipe(digest,sizeof(digest));
    if (!got) return Kq05TrackerBatchVerifyResult::kCryptoFailure;
    if (diff!=0U) return Kq05TrackerBatchVerifyResult::kStoreUnavailable;
  }
  proven.verified_=true;
  output=proven;
  return Kq05TrackerBatchVerifyResult::kAuthenticated;
}

} // namespace orun_tlp

#endif
