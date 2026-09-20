# ORUN Downlink Rendezvous Timing Plan

Status: **DRAFT DESIGN — documentation only; no runtime or wire change authorized.**

Baseline: `main@868b622cd07e3fe018b9f612ebc4455819a53e4f` (PR #24 merged).

This document records the next RF-design question after the M5 duplicate invariant
and the nominal field planning reference of approximately 15 TRACKER / 4
relay-forwarding nodes / 2 gateway bridges + optional 1 MOBILE gateway.

## 1. Purpose

Define the timing and path-selection contract that a future sleeping TRACKER
would need in order to receive a response after its own uplink, without changing
TLP v1 bytes, weakening current relay redundancy, or guessing a final RX-window
duration before the future response frame size/security overhead is known.

This document does **not** implement ACK, COMMAND, MESSAGE, secure envelope,
gateway coordination, downlink relay forwarding or a new packet type.

## 2. Frozen current invariants

Current behavior remains unchanged:

- TLP v1 POSITION is 34 bytes.
- TLP v1 RELAY_FORWARD is 49 bytes and preserves the exact original POSITION.
- Current TLP v1 relay forwarding remains one RF relay hop; nested
  RELAY_FORWARD is rejected.
- Uplink relay delay remains 1200–4200 ms inclusive.
- Each relay that admits a direct TRACKER POSITION keeps its own independent
  one-time forward attempt.
- Hearing another relay's copy does not cancel that already-scheduled attempt.
- A repeated direct `(source_device_id, sequence_number)` is suppressed while
  the key remains in that relay's bounded dedupe cache.
- BASE/application dedupe remains by original source + sequence.
- TRACKER post-TX RX remains `kWindowedRxAfterTxMs = 3000` ms until a later
  implementation milestone deliberately changes it.
- `TX_DONE` remains local radio completion, never delivery/contact evidence.

## 3. Current timing facts

At the current SF11 / BW125 / CR4/5 / preamble-8 PHY, the existing airtime
estimates are:

| Frame | Bytes | Estimated airtime |
| --- | ---: | ---: |
| POSITION | 34 | 987.136 ms |
| RELAY_FORWARD | 49 | 1232.896 ms |

The TRACKER post-TX listen window starts after its local TX terminal event. A
relay that has just received that POSITION may wait 1200–4200 ms before starting
its RELAY_FORWARD.

Ignoring small owner-loop/IRQ processing latency, the existing relay path can
therefore complete at the gateway approximately:

```text
minimum: 1.200 s + 1.232896 s = 2.432896 s
maximum: 4.200 s + 1.232896 s = 5.432896 s
```

after the TRACKER TX completes.

Therefore the current 3-second TRACKER window is not sufficient to guarantee that
a gateway can first receive a worst-case relayed uplink and still return a
response through the same rendezvous opportunity. This is a timing conclusion,
not evidence that the current 3-second M6P1 behavior is incorrect for its
original power-saving purpose.

## 4. Future return-path direction

For a future authenticated response, use an **ephemeral selected reverse path**
for that message attempt rather than asking every relay to perform a fast
downlink retransmission.

### Direct ingress

If a selected gateway has a usable direct observation of the TRACKER uplink, a
future policy may send the response directly to the TRACKER.

### Relayed ingress

If the selected gateway received the uplink through relay R1:

```text
TRACKER -> R1 -> GATEWAY
```

then R1 is the natural first reverse-path candidate for that response attempt:

```text
GATEWAY -> R1 -> TRACKER
```

R1 has just demonstrated both relevant uplink legs for that attempt:
TRACKER->R1 and R1->GATEWAY. The reverse path is still not guaranteed to be
symmetric, so failure/retry policy remains necessary later.

Only the selected relay forwards that specific downlink attempt. Other relays do
not all race to forward the same fast response. This avoids converting a
latency-sensitive return path into four overlapping transmissions.

This is **not** a permanent route table, primary-relay assignment, role change or
ownership relationship. It is per-message infrastructure path selection.

## 5. Timing budget

For a response returned through the selected ingress relay, the required tracker
listen interval must satisfy:

```text
T_window >=
    D_uplink_relay
  + A_RELAY_FORWARD
  + T_gateway_processing
  + A_gateway_to_relay_downlink
  + T_selected_relay_turnaround
  + A_relay_to_tracker_downlink
  + T_guard
```

where:

- `D_uplink_relay` is currently at most 4200 ms;
- `A_RELAY_FORWARD` is currently 1232.896 ms;
- the future downlink airtimes depend on the actual reviewed wire format;
- gateway processing and relay turnaround must be bounded by implementation;
- `T_guard` covers scheduler/owner-loop/radio-state margin and measured jitter.

A final `kWindowedRxAfterTxMs` value must not be chosen until those unknowns are
bounded.

### Engineering examples only

These examples are not protocol commitments.

If both future downlink RF legs were no larger than the current 49-byte
RELAY_FORWARD airtime, gateway processing were <=100 ms and the selected relay
turnaround were <=300 ms:

```text
4.200 + 1.232896 + 0.100 + 1.232896 + 0.300 + 1.232896
= 8.298688 s before guard margin
```

If both future downlink legs were approximately 80 bytes at the same PHY
(estimated airtime about 1.806336 s each):

```text
4.200 + 1.232896 + 0.100 + 1.806336 + 0.300 + 1.806336
= 9.445568 s before guard margin
```

A 10-second window is therefore only a **candidate** if the final frame size,
processing bounds and measured guard margin support it. It is not approved by
this document.

## 6. Why not use the normal 1200–4200 ms delay again on the return leg?

The uplink delay spreads several independent relay copies and preserves path
redundancy. A selected reverse path has a different job: one chosen relay should
return one latency-sensitive response while the TRACKER is still awake.

Reusing the full uplink delay on the return leg would consume much of the
TRACKER's RX window for no collision-avoidance benefit once only one relay has
been selected.

Therefore:

```text
uplink relay scheduling != selected downlink relay turnaround
```

The exact downlink turnaround bound remains an implementation decision after the
wire format and radio-owner behavior are specified and tested.

## 7. Multiple gateways

Multiple gateways receiving the same uplink is useful reception diversity.

For a future downlink attempt, however, this timing model assumes **one gateway
origin has been selected** before RF transmission. Two gateways independently
sending the same response can collide or unnecessarily amplify airtime.

Gateway-selection/coordination is intentionally outside this timing slice. It
must remain separate from legacy BASE role, relay forwarding, DeviceIdentity and
user ownership. Offline operation must be preserved by the later gateway design;
a live Internet dependency must not be introduced merely to choose a downlink
path.

## 8. Security and delivery semantics

No unauthenticated response may become trusted network-contact evidence or
control state.

Before a future response can mean receipt/contact, configuration acceptance,
command acceptance/result or another protected action, the reviewed security
architecture requires authentication, authorization where applicable,
anti-replay, durable nonce/counter behavior and explicit message identity.

A relay should normally forward an opaque protected message without becoming the
tracker's credential owner or application authority.

Normal telemetry still does not require an ACK for every packet. Critical
traffic may use bounded ACK/retry policy later.

## 9. Implementation gates

Before production code changes the 3-second tracker window or introduces a
downlink frame:

1. approve the secure envelope / response family and exact maximum wire length;
2. define stable message identity, destination and retry semantics;
3. define how one gateway origin and, for relayed return, one ingress relay are
   selected for a given attempt;
4. calculate airtime using the exact final frame lengths and current/configured
   PHY;
5. host-test the timing state machine, monotonic rollover, timeout/retry and
   mixed direct/relay cases;
6. model the nominal 15 TRACKER / 4 RELAY / 2 GATEWAY (+ optional MOBILE)
   collision domain, including critical/backlog traffic;
7. run the full host suite and RAK4630 PlatformIO build;
8. physically verify TRACKER->RELAY->GATEWAY->RELAY->TRACKER timing on owned
   hardware before calling the return path PASS;
9. measure current later before making battery-life claims for a longer RX
   window.

## 10. Non-claims

This document does not claim:

- that 10 seconds is the final tracker RX window;
- that a downlink packet currently exists;
- that a current relay forwards gateway traffic toward a tracker;
- that current TLP v1 provides authenticated ACK/contact;
- that multiple gateways are currently coordinated;
- that reverse RF links are always symmetric;
- that the nominal 15/4/2(+1) topology is physically validated.

Current M5/M6P1 runtime and all frozen compatibility fixtures remain unchanged.
