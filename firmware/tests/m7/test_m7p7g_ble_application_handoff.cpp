#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "ble_application_handoff.h"

using orun_tlp::BleApplicationConfirmationEvent;
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

  // Exactly one fixed-size ingress event can be queued; newer work does not
  // overwrite the earlier callback event.
  uint8_t frame_a[20];
  for (uint8_t i = 0; i < sizeof(frame_a); ++i) frame_a[i] = i;
  assert(h.enqueueIngress(7, frame_a, sizeof(frame_a)));
  uint8_t frame_b[8] = {1,2,3,4,5,6,7,8};
  assert(!h.enqueueIngress(7, frame_b, sizeof(frame_b)));
  assert(!h.enqueueIngress(8, frame_b, sizeof(frame_b)));
  assert(!h.enqueueIngress(7, frame_b, 21));

  BleApplicationIngressEvent in;
  assert(h.takeIngress(in));
  assert(in.session_generation == 11);
  assert(in.connection_handle == 7);
  assert(in.frame_len == sizeof(frame_a));
  assert(memcmp(in.frame, frame_a, sizeof(frame_a)) == 0);
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
  assert(!h.enqueueConfirmation(7, 0x1234));
  assert(!h.enqueueConfirmation(8, 0x1234));

  BleApplicationConfirmationEvent confirm;
  assert(h.takeConfirmation(confirm));
  assert(confirm.session_generation == 11);
  assert(confirm.connection_handle == 7);
  assert(confirm.value_handle == 0x1234);
  assert(!h.takeConfirmation(confirm));

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

  // Exact disconnect cleanup closes admission and clears all queued work.
  assert(h.enqueueIngress(9, frame_b, sizeof(frame_b)));
  assert(h.enqueueConfirmation(9, 0x3333));
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

  return 0;
}
