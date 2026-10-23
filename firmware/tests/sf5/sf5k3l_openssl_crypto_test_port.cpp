// TEST-ONLY OpenSSL implementation of the same production crypto port.
// Never build this file in the RAK4630 firmware environment.
#include "custody_ack_primitives.h"

#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <string.h>

namespace orun_tlp {
namespace custody_crypto {

bool sha256(const uint8_t* data,size_t length,uint8_t (&out)[32]) {
  if(data==nullptr || length==0U || length>100U)return false;
  uint8_t tmp[SHA256_DIGEST_LENGTH]{};
  if(SHA256(data,length,tmp)==nullptr)return false;
  memcpy(out,tmp,sizeof(tmp));
  OPENSSL_cleanse(tmp,sizeof(tmp));
  return true;
}

bool hkdfSha256(const uint8_t (&salt)[16],const uint8_t (&root)[32],
                const uint8_t* info,size_t info_len,uint8_t (&out)[32]) {
  if(info==nullptr || info_len!=57U)return false;
  EVP_PKEY_CTX* ctx=EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF,nullptr);
  if(ctx==nullptr)return false;
  uint8_t tmp[32]{};
  size_t n=sizeof(tmp);
  bool ok=EVP_PKEY_derive_init(ctx)==1 &&
          EVP_PKEY_CTX_set_hkdf_md(ctx,EVP_sha256())==1 &&
          EVP_PKEY_CTX_set1_hkdf_salt(ctx,salt,sizeof(salt))==1 &&
          EVP_PKEY_CTX_set1_hkdf_key(ctx,root,sizeof(root))==1 &&
          EVP_PKEY_CTX_add1_hkdf_info(ctx,info,info_len)==1 &&
          EVP_PKEY_derive(ctx,tmp,&n)==1 && n==sizeof(tmp);
  if(ok)memcpy(out,tmp,sizeof(tmp));
  OPENSSL_cleanse(tmp,sizeof(tmp));
  EVP_PKEY_CTX_free(ctx);
  return ok;
}

bool hmacSha256(const uint8_t (&key)[32],const uint8_t* data,
                size_t length,uint8_t (&out)[32]) {
  if(data==nullptr || length!=102U)return false;
  unsigned int n=0U;
  uint8_t tmp[EVP_MAX_MD_SIZE]{};
  const bool ok=HMAC(EVP_sha256(),key,sizeof(key),
                     data,length,tmp,&n)!=nullptr && n==32U;
  if(ok)memcpy(out,tmp,32U);
  OPENSSL_cleanse(tmp,sizeof(tmp));
  return ok;
}
} // namespace custody_crypto
} // namespace orun_tlp
