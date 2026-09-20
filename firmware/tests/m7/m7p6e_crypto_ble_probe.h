#pragma once

#include <stdint.h>

namespace orun_tlp {
namespace m7p6e_test {

struct KatResult {
  bool pass = false;
  bool hkdf_d2a = false;
  bool hkdf_a2d = false;
  bool direction_separated = false;
  bool nonce_layout = false;
  bool ccm_encrypt = false;
  bool ccm_decrypt = false;
  bool tamper_rejected = false;
  bool recovery = false;
  uint32_t tamper_result = 0;
};

KatResult runCandidateKat();

}  // namespace m7p6e_test
}  // namespace orun_tlp
