#pragma once

namespace orun_tlp::m7p6i_test {

struct HistoryCryptoKatResult {
  bool provision = false;
  bool observation = false;
  bool receipt = false;
  bool tamper_rejected = false;
  bool recovery = false;

  bool pass() const {
    return provision && observation && receipt &&
           tamper_rejected && recovery;
  }
};

HistoryCryptoKatResult runHistoryCryptoKat();

}  // namespace orun_tlp::m7p6i_test
