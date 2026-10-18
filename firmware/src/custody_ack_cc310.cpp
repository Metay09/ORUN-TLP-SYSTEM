// Production-only CryptoCell CC310 primitive provider for SF5K3L.
// Runs under the existing Bluefruit/CC310 lifecycle; NEVER initializes,
// re-enters, or shuts down nRFCrypto independently inside a radio callback.
// The verified caller is loop-owner-only and does not publish any K_root.
#include "custody_ack_primitives.h"

#include <string.h>
#include <nrf_cc310/include/crys_error.h>
#include <nrf_cc310/include/crys_hash.h>
#include <nrf_cc310/include/crys_hkdf.h>
#include <nrf_cc310/include/crys_hmac.h>

namespace orun_tlp {
namespace custody_crypto {
namespace {
void wipe(void* data,size_t size) {
  volatile uint8_t* p=static_cast<volatile uint8_t*>(data);
  while(size--!=0U) *p++=0U;
}
} // namespace

bool sha256(const uint8_t* data,size_t length,uint8_t (&digest)[32]) {
  if (data==nullptr || length==0U || length>100U) return false;
  uint8_t input[100]{};
  CRYS_HASH_Result_t raw{};
  memcpy(input,data,length);
  const CRYSError_t rc=CRYS_HASH(CRYS_HASH_SHA256_mode,input,length,raw);
  if (rc==CRYS_OK) memcpy(digest,raw,sizeof(digest));
  wipe(input,sizeof(input));
  wipe(raw,sizeof(raw));
  return rc==CRYS_OK;
}

bool hkdfSha256(const uint8_t (&salt)[16],const uint8_t (&root)[32],
                const uint8_t* info,size_t length,uint8_t (&key)[32]) {
  if(info==nullptr || length!=57U)return false;
  uint8_t scratch_salt[16]{};
  uint8_t scratch_root[32]{};
  uint8_t scratch_info[57]{};
  uint8_t derived[32]{};
  memcpy(scratch_salt,salt,sizeof(scratch_salt));
  memcpy(scratch_root,root,sizeof(scratch_root));
  memcpy(scratch_info,info,sizeof(scratch_info));
  const CRYSError_t rc=CRYS_HKDF_KeyDerivFunc(
      CRYS_HKDF_HASH_SHA256_mode,
      scratch_salt,sizeof(scratch_salt),
      scratch_root,sizeof(scratch_root),
      scratch_info,sizeof(scratch_info),
      derived,sizeof(derived),SASI_FALSE);
  if(rc==CRYS_OK)memcpy(key,derived,sizeof(derived));
  wipe(scratch_salt,sizeof(scratch_salt));
  wipe(scratch_root,sizeof(scratch_root));
  wipe(scratch_info,sizeof(scratch_info));
  wipe(derived,sizeof(derived));
  return rc==CRYS_OK;
}

bool hmacSha256(const uint8_t (&key)[32],const uint8_t* data,
                size_t length,uint8_t (&mac)[32]) {
  if(data==nullptr || length!=102U)return false;
  uint8_t scratch_key[32]{};
  uint8_t scratch_data[102]{};
  CRYS_HASH_Result_t result{};
  memcpy(scratch_key,key,sizeof(scratch_key));
  memcpy(scratch_data,data,sizeof(scratch_data));
  const CRYSError_t rc=CRYS_HMAC(
      CRYS_HASH_SHA256_mode,scratch_key,sizeof(scratch_key),
      scratch_data,sizeof(scratch_data),result);
  if(rc==CRYS_OK)memcpy(mac,result,sizeof(mac));
  wipe(scratch_key,sizeof(scratch_key));
  wipe(scratch_data,sizeof(scratch_data));
  wipe(result,sizeof(result));
  return rc==CRYS_OK;
}

} // namespace custody_crypto
} // namespace orun_tlp
