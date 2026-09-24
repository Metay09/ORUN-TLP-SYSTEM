#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "ble_application_handoff.h"

using orun_tlp::BleApplicationConfirmationEvent;
using orun_tlp::BleApplicationGattTimeoutEvent;
using orun_tlp::BleApplicationHandoff;
using orun_tlp::BleApplicationIngressEvent;

int main() {
  BleApplicationHandoff h;

  // Zero generation is never a valid callback-visible session.
  h.activateSession(7, 0);
  assert(!h.sessionActive());
  uint8_t one = 0xAA;
  assert(!h.enqueueIngress(7, &one, 1));

  h.activateSession(7, 11);
  assert(h.sessionActive());
  assert(h.ingressAllowed());
  assert(h.connectionHandle() == 7);
  assert(h.sessionGeneration() == 11);

  // A complete four-fragment M7P7F request burst fits even if Bluefruit
  // delivers ATT write callbacks faster than the cooperative loop consumes
  // them. The fifth queued frame is rejected and FIFO order is preserved.
  uint8_t frame_a[20];
  for (uint8_t i = 0; i < sizeof(frame_a); ++i) frame_a[i] = i;
  uint8_t frame_b[8] = {1,2,3,4,5,6,7,8};
  uint8_t frame_c[8] = {9,10,11,12,13,14,15,16};
  uint8_t frame_d[8] = {17,18,19,20,21,22,23,24};
  uint8_t frame_e[8] = {25,26,27,28,29,30,31,32};
  assert(h.enqueueIngress(7, frame_a, sizeof(frame_a)));
  assert(h.enqueueIngress(7, frame_b, sizeof(frame_b)));
  assert(h.enqueueIngress(7, frame_c, sizeof(frame_c)));
  assert(h.enqueueIngress(7, frame_d, sizeof(frame_d)));
  assert(!h.enqueueIngress(7, frame_e, sizeof(frame_e)));
  assert(!h.enqueueIngress(8, frame_e, sizeof(frame_e)));
  assert(!h.enqueueIngress(7, frame_e, 21));

  BleApplicationIngressEvent in;
  BleApplicationConfirmationEvent confirm;
  assert(h.takeIngress(in));
  assert(in.session_generation == 11);
  assert(in.connection_handle == 7);
  assert(in.frame_len == sizeof(frame_a));
  assert(memcmp(in.frame, frame_a, sizeof(frame_a)) == 0);
  assert(h.takeIngress(in));
  assert(memcmp(in.frame, frame_b, sizeof(frame_b)) == 0);
  assert(h.takeIngress(in));
  assert(memcmp(in.frame, frame_c, sizeof(frame_c)) == 0);
  assert(h.takeIngress(in));
  assert(memcmp(in.frame, frame_d, sizeof(frame_d)) == 0);
  assert(!h.takeIngress(in));

  // Zero-length malformed wire writes are still bounded events; the transport
  // layer, not the callback, owns frame syntax rejection.
  assert(h.enqueueIngress(7, nullptr, 0));
  assert(h.takeIngress(in));
  assert(in.frame_len == 0);

  // Stop-and-wait is visible in callback context. Closing the gate clears any
  // queued next request, so it cannot execute later after confirmation.
  assert(h.enqueueIngress(7, frame_b, sizeof(frame_b)));
  h.setIngressAllowed(7, 11, false);
  assert(!h.ingressAllowed());
  assert(!h.takeIngress(in));
  assert(!h.enqueueIngress(7, frame_b, sizeof(frame_b)));

  // Stale session controls are harmless.
  h.setIngressAllowed(7, 10, true);
  assert(!h.ingressAllowed());
  h.deactivateSession(7, 10);
  assert(h.sessionActive());

  h.setIngressAllowed(7, 11, true);
  assert(h.ingressAllowed());
  assert(h.enqueueConfirmation(7, 0x1234));
  // Exact HVC provisionally reopens callback admission so a peer may issue
  // the next ATT write immediately after confirming the indication.
  assert(h.ingressAllowed());
  assert(h.enqueueIngress(7, frame_b, sizeof(frame_b)));
  assert(!h.enqueueConfirmation(7, 0x1234));
  assert(!h.enqueueConfirmation(8, 0x1234));

  // Regression: loop may have sampled the old outbound-pending state just
  // before callback HVC + immediate next WRITE. That stale close must not
  // erase a post-HVC request while the confirmation fact is still pending.
  h.setIngressAllowed(7, 11, false);
  assert(h.ingressAllowed());
  assert(h.takeConfirmation(confirm));
  assert(confirm.session_generation == 11);
  assert(confirm.connection_handle == 7);
  assert(confirm.value_handle == 0x1234);
  assert(!h.takeConfirmation(confirm));
  h.setIngressAllowed(7, 11, true);
  assert(h.takeIngress(in));
  assert(in.frame_len == sizeof(frame_b));

  // Negative side: once the confirmation has been consumed, a close decision
  // is authoritative again and must clear queued ingress. This models a stale
  // or otherwise non-matching HVC that loop did not accept for its in-flight
  // response.
  assert(h.enqueueIngress(7, frame_b, sizeof(frame_b)));
  h.setIngressAllowed(7, 11, false);
  assert(!h.ingressAllowed());
  assert(!h.takeIngress(in));
  assert(!h.enqueueIngress(7, frame_b, sizeof(frame_b)));

  h.setIngressAllowed(7, 11, true);

  // Replacement session invalidates queued old-session ingress and HVC.
  assert(h.enqueueIngress(7, frame_b, sizeof(frame_b)));
  assert(h.enqueueConfirmation(7, 0x2222));
  h.activateSession(9, 12);
  assert(h.sessionActive());
  assert(h.connectionHandle() == 9);
  assert(h.sessionGeneration() == 12);
  assert(!h.takeIngress(in));
  assert(!h.takeConfirmation(confirm));
  assert(!h.enqueueIngress(7, frame_b, sizeof(frame_b)));

  // A protocol-source GATTS timeout is terminal for the active session's ATT
  // progress: callback admission closes immediately and queued ingress/HVC
  // facts are discarded before loop-owned teardown/disconnect recovery.
  assert(h.enqueueIngress(9, frame_b, sizeof(frame_b)));
  assert(h.enqueueConfirmation(9, 0x3333));
  assert(!h.enqueueGattTimeout(8));
  assert(h.enqueueGattTimeout(9));
  assert(!h.ingressAllowed());
  assert(!h.takeIngress(in));
  assert(!h.takeConfirmation(confirm));
  assert(!h.enqueueIngress(9, frame_b, sizeof(frame_b)));
  assert(!h.enqueueConfirmation(9, 0x3333));
  assert(!h.enqueueGattTimeout(9));

  BleApplicationGattTimeoutEvent timeout;
  assert(h.takeGattTimeout(timeout));
  assert(timeout.session_generation == 12);
  assert(timeout.connection_handle == 9);
  assert(!h.takeGattTimeout(timeout));

  // Exact disconnect cleanup closes admission and clears all queued work.
  h.deactivateSession(9, 12);
  assert(!h.sessionActive());
  assert(!h.ingressAllowed());
  assert(!h.takeIngress(in));
  assert(!h.takeConfirmation(confirm));

  // Fresh session remains usable after cleanup.
  h.activateSession(9, 13);
  assert(h.enqueueIngress(9, frame_b, sizeof(frame_b)));
  assert(h.takeIngress(in));
  assert(in.session_generation == 13);

  // A replacement session also clears an unconsumed old timeout fact.
  assert(h.enqueueGattTimeout(9));
  h.activateSession(10, 14);
  assert(!h.takeGattTimeout(timeout));
  assert(h.ingressAllowed());
  assert(h.enqueueIngress(10, frame_b, sizeof(frame_b)));

  return 0;
}
