# M7P7F Independent Follow-up Review Disposition

Status: **FIXES APPLIED — OWNER REVALIDATION PENDING**

Reviewed head: `afd2e9232b404dcde40b4600fb2ec14f37a793f6`

Scope: M7P7F BLE application transport/session contract only. This review does
not claim physical BLE/GATT validation because M7P7F remains pre-wire and
`main.cpp` does not instantiate the transport.

## Findings

### MEDIUM — old-session queued events were not fully generation-gated

`endSession(generation)` rejected stale disconnects, but
`onFrameReceived()`, outbound peek and `confirmOutboundFrame()` did not carry
session provenance. A callback-derived frame or indication confirmation queued
for session A could therefore be processed after session B replaced it.

Disposition: **fixed**. Inbound frame delivery, outbound peek, indication
confirmation and disconnect cleanup all require the captured non-zero session
generation. Stale/inactive generations return before touching current-session
state.

### MEDIUM — stop-and-wait started too late

The pre-review code checked `outbound_.pending` only in
`dispatchInbound()`, after a logical request had fully reassembled. A peer
could start and retain a partial request while response A awaited indication
confirmation, then complete that request immediately after A was confirmed.

Disposition: **fixed**. While an outbound response is pending,
`onFrameReceived()` rejects ingress before reassembly and clears any partial
RX state. Host coverage proves a START-only next request cannot be staged and a
later continuation cannot resurrect it.

### LOW — session generation could pass through zero / no explicit active state

Disposition: **fixed**. Session generations skip zero and the transport stores
one bounded `session_active_` bit. Session-derived operations require both an
active session and an exact generation match.

## Compatibility impact

- TLP v1 bytes/sizes: unchanged.
- RF/relay behavior: unchanged.
- HistoryStore/ConfigStore/SecurityStore formats: unchanged.
- BLE admission/advertising/bond behavior: unchanged.
- Bluefruit GATT runtime: still not implemented.
- ApplicationRequester remains local adapter provenance; peer bytes cannot
  select it.
- No heap or unbounded queue was added.

## Validation state

The earlier host/PlatformIO PASS and zero linked RAM/flash delta belong to the
pre-review head `afd2e923...`. Because the fixes change transport code and
tests, fresh owner-run focused/full host validation and the production
`rak4630` PlatformIO build are required before merge. No post-fix PASS is
claimed by this document yet.
