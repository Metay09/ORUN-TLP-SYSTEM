#pragma once

namespace orun_tlp::sf5b_test {

struct ProductCryptoKatResult {
  bool unprovisioned_refused = false;
  bool provision = false;
  bool periodic = false;
  bool event = false;
  bool refused_without_counter = false;

  bool pass() const {
    return unprovisioned_refused && provision && periodic && event &&
           refused_without_counter;
  }
};

// TEST-ONLY: production ProductSecureCrypto + CC310 against the pinned KAT
// frames of sf5b_product_vectors.h, on a RAM-backed SecurityStore with the
// public test credential. Never touches the physical SecurityStore.
ProductCryptoKatResult runProductCryptoKat();

}  // namespace orun_tlp::sf5b_test
