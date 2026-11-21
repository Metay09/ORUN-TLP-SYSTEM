#include "kq05_gateway_batch_custody_crypto.h"

#include <string.h>

#include "custody_ack.h"
#include "custody_ack_primitives.h"
#include "tlp_v2_product_secure.h"

namespace orun_tlp {
namespace {
namespace ack = kq05_gateway_batch_ack;

struct Digest {
  uint8_t full[32]{};
};

void zero(void* memory, size_t bytes) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *p++ = 0U;
}

bool equalConstantTime(const uint8_t* a, const uint8_t* b, size_t size) {
  volatile uint8_t difference = 0U;
  for (size_t i = 0U; i < size; ++i)
    difference = static_cast<uint8_t>(difference | (a[i] ^ b[i]));
  return difference == 0U;
}

bool sameScope(const ack::Header& a, const GatewayCustodyAckScope& b) {
  return a.tracker_device_id == b.tracker_device_id &&
         a.gateway_device_id == b.gateway_device_id &&
         a.key_epoch == b.key_epoch &&
         a.custody_floor == b.custody_floor &&
         a.grant_generation == b.grant_generation;
}

bool permittedScope(const GatewayCustodyAckScope& s) {
  return s.tracker_device_id != 0U &&
         s.tracker_device_id != UINT64_MAX &&
         s.gateway_device_id != 0U &&
         s.gateway_device_id != UINT64_MAX &&
         s.key_epoch != UINT32_MAX &&
         s.custody_floor != UINT32_MAX &&
         s.grant_generation != 0U &&
         s.grant_generation != UINT32_MAX;
}

bool collectSortedDigests(
    const GatewayCustodyAckScope& scope,
    const Kq05BatchExactView* objects, uint8_t count,
    Digest (&digests)[ack::kMaxObjects]) {
  if (!permittedScope(scope) || objects == nullptr ||
      count == 0U || count > ack::kMaxObjects) return false;

  for (uint8_t i = 0U; i < count; ++i) {
    custody_ack::Header shape{};
    shape.tracker_device_id = scope.tracker_device_id;
    shape.gateway_device_id = scope.gateway_device_id;
    shape.key_epoch = scope.key_epoch;
    shape.object_len = static_cast<uint8_t>(objects[i].size);
    if (!custody_ack::matchesProtectedShape(
            shape, objects[i].bytes, objects[i].size) ||
        !custody_crypto::sha256(
            objects[i].bytes, objects[i].size, digests[i].full))
      return false;
  }
  // Deterministic ordering by the 8-byte indexing hint, NOT arrival order.
  // Colliding prefixes can never be made into an ambiguous batch.
  for (uint8_t i = 1U; i < count; ++i) {
    Digest item = digests[i];
    uint8_t j = i;
    while (j > 0U &&
           memcmp(digests[j-1U].full, item.full,
                  ack::kRefSize) > 0) {
      digests[j] = digests[j-1U];
      --j;
    }
    digests[j] = item;
  }
  for (uint8_t i = 1U; i < count; ++i)
    if (memcmp(digests[i-1U].full, digests[i].full,
               ack::kRefSize) == 0) return false;
  return true;
}

bool batchMac(const uint8_t* wire, size_t length,
              const Digest (&digests)[ack::kMaxObjects], uint8_t count,
              const uint8_t (&key)[32], uint8_t (&mac)[32]) {
  if (wire == nullptr || count == 0U || count > ack::kMaxObjects ||
      length != ack::kHeaderSize + ack::kRefSize * count + ack::kTagSize)
    return false;
  uint8_t input[ack::kMaxMacInput]{};
  constexpr size_t domain_size = sizeof(ack::kMacDomain) - 1U;
  size_t used = 0U;
  memcpy(input, ack::kMacDomain, domain_size);
  used += domain_size;
  memcpy(input + used, wire, length - ack::kTagSize);
  used += length - ack::kTagSize;
  for (uint8_t i = 0U; i < count; ++i) {
    memcpy(input + used, digests[i].full, sizeof(digests[i].full));
    used += sizeof(digests[i].full);
  }
  const bool ok = used <= sizeof(input) &&
      custody_crypto::hmacSha256Bounded(key, input, used, mac);
  zero(input, sizeof(input));
  return ok;
}

} // namespace

bool Kq05GatewayBatchCustodyCrypto::signAfterDurableReadback(
    const GatewayCustodyAckScope& scope,
    const uint8_t (&scoped_key)[32],
    const Kq05BatchExactView* exact_objects,
    uint8_t count, uint8_t (&out)[ack::kMaxSize], size_t& written) {
  Digest digests[ack::kMaxObjects]{};
  if (!collectSortedDigests(scope, exact_objects, count, digests)) {
    zero(digests, sizeof(digests));
    return false;
  }
  ack::Header h{};
  h.tracker_device_id = scope.tracker_device_id;
  h.gateway_device_id = scope.gateway_device_id;
  h.key_epoch = scope.key_epoch;
  h.custody_floor = scope.custody_floor;
  h.grant_generation = scope.grant_generation;
  h.count = count;
  for (uint8_t i = 0U; i < count; ++i)
    memcpy(h.refs[i], digests[i].full, ack::kRefSize);

  uint8_t result[ack::kMaxSize]{};
  uint8_t blank_tag[ack::kTagSize]{};
  uint8_t mac[32]{};
  size_t generated = 0U;
  const bool ok = ack::encode(h, blank_tag, result,
                               sizeof(result), generated) &&
      batchMac(result, generated, digests, count, scoped_key, mac);
  if (ok) {
    memcpy(result + generated - ack::kTagSize, mac, ack::kTagSize);
    memcpy(out, result, generated);
    written = generated;
  }
  zero(digests, sizeof(digests));
  zero(mac, sizeof(mac));
  zero(result, sizeof(result));
  return ok;
}

bool Kq05GatewayBatchCustodyCrypto::verifyAgainstExactRetained(
    const uint8_t* wire, size_t wire_size,
    const GatewayCustodyAckScope& trusted_scope,
    const uint8_t (&scoped_key)[32],
    const Kq05BatchExactView* retained_objects,
    uint8_t count) {
  ack::Header header{};
  if (!ack::decode(wire, wire_size, header) ||
      count != header.count ||
      !sameScope(header, trusted_scope)) return false;

  Digest digests[ack::kMaxObjects]{};
  if (!collectSortedDigests(trusted_scope, retained_objects,
                            count, digests)) {
    zero(digests, sizeof(digests));
    return false;
  }
  for (uint8_t i = 0U; i < count; ++i)
    if (!equalConstantTime(
            header.refs[i], digests[i].full, ack::kRefSize)) {
      zero(digests, sizeof(digests));
      return false;
    }

  uint8_t mac[32]{};
  const bool computed = batchMac(
      wire, wire_size, digests, count, scoped_key, mac);
  const bool authenticated = computed && equalConstantTime(
      wire + wire_size - ack::kTagSize, mac, ack::kTagSize);
  zero(mac, sizeof(mac));
  zero(digests, sizeof(digests));
  return authenticated;
}

} // namespace orun_tlp
