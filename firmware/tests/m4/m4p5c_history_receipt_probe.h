#pragma once

namespace orun_tlp::m4p5c_test {

struct HistoryReceiptKatResult {
  bool history_ready = false;
  bool provision = false;
  bool opaque_started = false;
  bool busy_guard = false;
  bool replay_wait = false;
  bool applied = false;
  bool tamper_rejected = false;
  bool duplicate_rejected = false;

  bool pass() const {
    return history_ready && provision && opaque_started && busy_guard &&
           replay_wait && applied && tamper_rejected && duplicate_rejected;
  }
};

HistoryReceiptKatResult runHistoryReceiptKat();

}  // namespace orun_tlp::m4p5c_test
