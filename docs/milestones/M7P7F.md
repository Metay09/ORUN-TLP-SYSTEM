# M7P7F — BLE application transport contract + bounded session state

Status: **SOFTWARE PASS — host/sanitizer suite and RAK4630 production build PASS,
zero RAM/flash delta (unreferenced by production composition). NO BLUEFRUIT
GATT SERVICE/CHARACTERISTIC, NO PROTECTED WRITE PATH, NO PHYSICAL BLE TEST.**

Baseline: `main@f833a6d56de17b902bc26061a3791086a14f1cf4` (PR #33 / M7P6E
corrected fresh-pairing evidence merged; verified as the exact `origin/main`
head before this branch started, matching the task's expected SHA).
Branch: `feat/m7p7f-ble-app-transport-contract`.

## 1. Why this slice exists

M7P7E closed application requester/response ownership before a second
transport adapter exists. M7P7C's design boundary and M7P7D/M7P7E's remaining
gates both require the exact BLE application GATT byte contract and a bounded
adapter/session seam to be frozen and host-tested *before* any production
Bluefruit `BLEService`/`BLECharacteristic` is wired.

M7P7F does exactly that and nothing more: it freezes the first ORUN
application transport wire contract (UUIDs, frame header, fragmentation,
message types, error codes) and implements a portable, Bluefruit-free,
loop-owned `BleApplicationTransport` session/reassembly component, fully
host-tested against the real `ApplicationRequestService` + `ConfigStore`. It
does **not** instantiate BLE GATT runtime. That remains M7P7G.

## 2. Scope and exclusions

Added:

- `firmware/include/ble_application_transport.h` /
  `firmware/src/ble_application_transport.cpp`: the frozen wire contract
  (constants/enums) plus `BleApplicationTransport`, a bounded, Bluefruit-free
  C++ class driven by loop/task-owned code.
- `firmware/tests/m7/test_m7p7f_ble_application_transport.cpp`: focused host
  coverage using the real `ApplicationRequestService`/`ConfigStore`, matching
  the M7P7D/M7P7E testing pattern.
- One new line in `firmware/tests/run_host_tests.sh` compiling and running
  that test under the existing warnings-as-errors + ASan/UBSan discipline.

Explicitly **not** added, per the task's hard boundaries:

- no `BLEService`/`BLECharacteristic`/`BLEUuid` object anywhere in
  `main.cpp` or elsewhere in production composition;
- no change to advertising behavior, BLE admission/no-client timeout, or BLE
  bond behavior;
- no credential provisioning, no `K_root` readback, no protected config
  write path;
- no wiring of `BleApplicationTransport` into `main.cpp` — production startup
  composition is **byte-for-byte unchanged** by this slice (verified in
  section 7: the file compiles into the production graph but is discarded by
  the linker as dead code, since nothing references it, producing an exact
  **0-byte** RAM/flash delta versus the merged M7P7E baseline);
- no MESSAGE, command, Entity Registry, Android or backend work.

## 3. Frozen 128-bit UUIDs

Generated once (`python3 -m uuid` equivalent), never derived from
`DeviceIdentity` or any runtime value, and never generated again at runtime.
Defined as compile-time `constexpr uint8_t[16]` arrays in
`ble_app_transport` (`firmware/include/ble_application_transport.h`):

```text
ORUN application service:       0088f2c3-cc13-4b5d-b025-f7fcb7c71af2
ORUN application request char:  fbf521f6-ce25-4e6a-af6c-c76303803ee3
ORUN application response char: c0ba4044-1242-447b-9f79-e97aa0065ad3
```

The header stores each as 16 bytes in plain RFC 4122 order (matching the
printed string byte-for-byte). M7P7F does not instantiate a `BLEUuid`; a
future M7P7G adapter is responsible for whatever byte-order conversion
Bluefruit's `BLEUuid` constructor requires (typically the reverse/on-air
order) — what is frozen here is the 128-bit identity, not an in-memory byte
order.

M7P7G's planned characteristic properties (not implemented by this slice):

- request characteristic: **WRITE with response** only — no WRITE WITHOUT
  RESPONSE;
- response characteristic: **INDICATE** only — no READ-based polling, no
  unbounded NOTIFY stream.

## 4. Frame contract (frozen, 20-byte ATT MTU 23 budget)

```text
byte 0      transport version, exactly 0x01
byte 1      message type
byte 2      flags (bit0 START=0x01, bit1 END=0x02; all other bits invalid)
byte 3      fragment index
bytes 4..5  peer correlation id, little-endian uint16
bytes 6..7  total logical payload length, little-endian uint16
bytes 8..   fragment payload (0..12 bytes)
```

- frame length must be 8..20 bytes;
- maximum logical payload 48 bytes; maximum 4 fragments (12 bytes/fragment);
- first fragment must be `START`, index 0; single-frame messages use
  `START|END`, index 0;
- later fragments increment index by exactly one; version, message type,
  correlation id and total length must stay identical across all fragments
  of one logical message;
- `END` is valid only when accumulated payload length exactly equals the
  declared total length;
- a new `START` always invalidates/clears a different in-progress partial
  message before processing the new one;
- any other validation failure (short/long frame, wrong version, unknown
  flag bits, `>48` logical length, over-length fragment, missing `START`,
  non-zero index on `START`, duplicate/skipped fragment, mid-message
  correlation/type/length change, early/wrong-length `END`) fails closed:
  no application/storage/radio side effect, and the in-progress partial
  reassembly is conservatively cleared;
- no partially reassembled request survives a session end (disconnect);
- a bounded **2000 ms** fragment-reassembly timeout
  (`ble_app_transport::kFragmentTimeoutMs`) discards a stalled partial
  request; it is advanced only by loop-owned `poll(now)`, using the same
  wrap-safe `monotonic::elapsed()` semantics as the rest of the firmware —
  no background timer/thread.

Transport v1 stays capped at 20 bytes regardless of any phone-negotiated ATT
MTU; no MTU negotiation is required or assumed.

## 5. Message types and payload layouts (frozen)

```text
0x01  GET_CONFIG request   (logical payload length MUST be 0; normally one
                             8-byte START|END frame)
0x81  GET_CONFIG response  (logical payload length 10; fits one 18-byte frame)
0xFF  ERROR response       (logical payload length 2)
```

GET_CONFIG response payload:

```text
byte 0      application status (0x00 = OK)
byte 1      flags: bit0 = config backend ready, bit1 = committed record
            present; all other bits zero in v1
bytes 2..5  tracking_interval_seconds, little-endian uint32
bytes 6..9  battery_capacity_mah, little-endian uint32
```

ERROR response payload:

```text
byte 0  error code: 0x01 = UNSUPPORTED, 0x02 = BUSY
byte 1  offending message type
```

No config-write, provisioning, command or MESSAGE opcode exists. Malformed
transport frames never generate an application response (no
error-amplification path for arbitrary garbage) — only a syntactically
complete, correctly reassembled logical message reaches dispatch and can
produce an `ERROR` reply.

One explicit design decision beyond the literal frame-syntax rules: a
well-formed, fully reassembled `GET_CONFIG` request whose declared logical
length is **nonzero** violates the type's own frozen zero-length contract.
`BleApplicationTransport` treats this the same as a malformed frame — fail
closed, no reply, no application/storage effect — rather than answering with
`ERROR/UNSUPPORTED`, to avoid giving a malformed-but-well-framed peer a
reliable way to elicit a wire reply (host test 19).

## 6. Pre-authorization security classification

The only application operation this transport contract exposes is the
existing read-only `GET_CONFIG` (M7P7D/M7P7E). For this milestone, the three
currently-returned fields —

- `tracking_interval_seconds`
- `battery_capacity_mah`
- backend/default provenance (`config_backend_ready`,
  `config_has_committed_record`)

— are classified as non-secret local diagnostic/config metadata that may be
read before future ORUN application authorization exists.

This is a **narrow allowlist**, not a general rule that diagnostics, status,
location or configuration are safe pre-auth. Private location, messages,
credentials, configuration mutation and commands remain unavailable over
this transport. BLE connection != BLE bond != ORUN application
authorization (`docs/architecture/ORUN_CURRENT_ARCHITECTURE_RULES.md`
section 17). This document does not choose or invent the final
commissioning cryptographic ceremony; that remains a dedicated, reviewed
future slice that must also decide authority-key custody/recovery before any
protected write exists.

## 7. `BleApplicationTransport` — bounded adapter/session state

`firmware/include/ble_application_transport.h` /
`firmware/src/ble_application_transport.cpp` have **no Bluefruit/Arduino
dependency** and are driven entirely by loop/task-owned code (future
M7P7G). No heap allocation; no `std::vector`/`std::string`; fixed-size
members only (`uint8_t payload[48]` reassembly/outbound buffers). Compiles
cleanly under both the host `-std=c++17` sanitizer flags and the pinned
`-std=gnu++11` ARM toolchain mode.

Bounded state, matching this milestone's single-BLE-client scope exactly:

- one active session (a local monotonic `session_generation_` counter,
  never a wire field);
- one partial inbound logical message (`InboundReassembly`);
- one in-flight/just-submitted application request (synchronous —
  `ApplicationRequestService::submit()` completes immediately, so no
  separate async in-flight state is needed beyond the call itself);
- one bounded outbound logical response (`OutboundResponse`) plus its
  fragment cursor.

`ApplicationRequester::kBle` is always the value submitted to
`ApplicationRequestService`; it is hard-coded and **never** derived from any
peer-controlled byte (frame header, correlation id, message type or
payload) — there is no code path by which wire bytes could select a
requester.

Internal request-ID discipline (section 7/8.5.2 of the task): a **local**
`uint32_t next_local_request_id_` member, initialized to 1, advanced by the
free function `ble_app_transport::advanceRequestId()` (skips 0 on
wraparound), and **never reset** by `beginSession()`/`endSession()` — it
persists for the lifetime of the `BleApplicationTransport` object, so it
does not restart at 1 on every reconnect. It is kept completely separate
from the peer-supplied `uint16_t` correlation id, which is only ever echoed
back in the response frame and is never treated as identity or security
state.

### Session hygiene

- `beginSession()` returns a new non-zero `session_generation_` (wrap
  skips zero), marks the session active, and defensively clears any leftover
  inbound/outbound state and discards a `kBle`-owned
  global response (`ApplicationRequestService::discardResponse` is a no-op
  unless the pending response actually belongs to `kBle`, so a USB-owned
  response is never touched).
- `endSession(session_generation)` is a **no-op unless the session is active
  and the generation matches**. Inbound frame delivery, outbound peek and
  indication confirmation are generation-gated by the same rule. A delayed
  frame/confirmation/disconnect from an already-replaced session therefore
  cannot observe, advance or clear the replacement session's state. A matching
  disconnect clears partial inbound reassembly, clears the unsent outbound
  response, discards a `kBle`-owned global response, and marks the session
  inactive.
- Connection-handle/session state is transport correlation only; it is not
  user identity or authorization. The generation is local bookkeeping, never
  sent on the wire.
- The design assumes M7P7G calls `endSession()` before the admission layer
  is allowed to re-advertise for the next application connection; no
  service-level multi-session framework is added, matching the current
  one-BLE-client-at-a-time product scope.

### Stop-and-wait backpressure

Only one outbound logical result buffer exists. A BLE client must not begin
a new application request while a prior response awaits indication
confirmation:

- if `outbound_.pending` is true, ingress is rejected **before reassembly
  starts**. No partial next request can be staged while the prior response
  awaits confirmation, and no second application submit/reply is generated
  (there is no second slot to hold one);
- once the global slot is free and a request is accepted,
  `BleApplicationTransport` takes the result into its own bounded buffer
  **immediately** (synchronously, in the same call) and releases the global
  `ApplicationRequestService` slot right away — a non-reading BLE client can
  therefore never wedge the shared slot, and USB (or any future adapter)
  remains free to use it while the BLE response still awaits indication
  (host tests 7-9);
- if the global slot is already held by another requester when BLE
  attempts to submit, `submit()` returns `kBusy` and
  `BleApplicationTransport` builds a local `ERROR/BUSY` reply itself — it
  never touches the other requester's pending response.

### Outbound indication API (for M7P7G)

- `outboundFramePending()` / `peekOutboundFrame()` / `confirmOutboundFrame()`.
- `peekOutboundFrame()` is a pure, repeatable peek: it does not advance
  state, so a failed/unconfirmed indication can be retried with the
  identical frame.
- `confirmOutboundFrame()` advances the fragment cursor and clears the whole
  logical response **only** once its final fragment has been confirmed.
- No Bluefruit call exists anywhere in this class.

## 8. Malformed-input coverage

`firmware/tests/m7/test_m7p7f_ble_application_transport.cpp` covers, each
verified to produce no application/storage side effect:

frame `<8` bytes; frame `>20` bytes; wrong version; unknown flag bits;
logical length `>48`; fragment payload exceeding declared total; missing
`START`; `START` with non-zero fragment index; unexpected new `START`
during a partial message (clears the old one, then correctly dispatches the
new one on its own terms); duplicate fragment; skipped fragment;
correlation-id change mid-message; message-type change mid-message;
total-length change mid-message; `END` too early / `END` with wrong final
length; exact declared length reached without `END` (stays pending, not
rejected, until timeout); fragment timeout (2000 ms, via loop-owned
`poll()`); disconnect with a partial request
(`endSession()` while `inboundReassemblyActive()`); the 4-fragment ceiling
enforced independently of the 48-byte length ceiling (5 small fragments for
a 20-byte logical length is rejected on the 5th); a nonzero-length
`GET_CONFIG` request (section 5); and a 200-frame flood of garbage followed
by one legitimate request, proving the fixed-size state machine never
wedges and never grows unbounded state.

## 9. Application/session host tests

The same file additionally covers (numbers match the task's required list):

1-2. single-frame `GET_CONFIG` round trip with exact little-endian wire
bytes checked field-by-field.
3. stored vs. default `ConfigStore` provenance bits, read through the real
`ConfigStore` owner (no duplicated application-side cache), exactly like
`test_m7p7d_app_request.cpp`.
4. a well-formed but unsupported message type -> `ERROR/UNSUPPORTED` with
the offending type echoed, no application dispatch.
5-6. global `ApplicationRequestService` BUSY (held by USB) -> local
`ERROR/BUSY`; BLE can never `takeResponse`/`discardResponse` the USB-owned
result, which remains intact and USB-readable afterward.
7-8. BLE takes its response into the local buffer promptly (global slot
free again immediately); USB can subsequently use the shared slot while the
BLE response still awaits indication.
9. a non-reading BLE client cannot overwrite its own pending response or
pre-stage a partial next request before the current indication is confirmed.
10-11. indication retry returns byte-identical frames until confirmed;
the response is removed only once its (here, only) fragment is confirmed.
12-13. disconnect clears the outbound response; a subsequent reconnect
starts with nothing pending.
14. delayed old-session frame delivery, outbound peek, indication
confirmation and disconnect cannot observe/advance/clear the replacement
session's response.
15-16. the same peer `uint16` correlation id repeating in a different
session is safe (echoed correctly per-session); the local `uint32` request
id keeps advancing across reconnect rather than resetting to 1.
17. request-id and session-generation wraparound both skip zero, tested as
pure helpers rather than by executing 2^32 operations.
18. a disconnect mid-fragment-reassembly leaves no lingering inbound state.
19. the malformed/flood matrix in section 8 above.

## 10. Known limits of this test coverage

All currently frozen response payloads (`GET_CONFIG` response 10 bytes,
`ERROR` 2 bytes) fit in a single 20-byte frame, so the outbound
fragmentation cursor (`peekOutboundFrame`/`confirmOutboundFrame` advancing
across multiple fragments) is only exercised as a single-fragment case
through the real dispatch path. Multi-fragment **inbound** reassembly
mechanics (index increment, duplicate/skip/mismatch rejection, the
4-fragment ceiling) are exercised directly using a non-`GET_CONFIG` message
type carrying a longer synthetic payload, since `GET_CONFIG` itself is
frozen at a zero-length request. The general outbound chunking arithmetic
(`offset`/`chunk` computation) is the same code path regardless of which
message populated `outbound_`, but a true multi-fragment *response*
sequence has no reachable trigger until a future message type with a
payload longer than 12 bytes is frozen.

## 11. Compatibility / system impact

```text
TLP v1 bytes/sizes:             unchanged
RF PHY/airtime/forwarding:      unchanged
HistoryStore bytes:             unchanged
ConfigStore bytes:              unchanged
SecurityStore bytes:            unchanged
BLE runtime/admission:          unchanged
BLE application GATT:           not implemented (M7P7G)
provisioning/authorization:     not implemented
durable config write over APP:  not implemented
USB APP CONFIG? text shape:     unchanged
MESSAGE/commands:                not implemented
Android/backend:                not implemented
main.cpp:                       unchanged (BleApplicationTransport is not
                                 instantiated anywhere in production
                                 composition)
```

## 12. Validation evidence

Pre-audit owner-run validation on exact head `afd2e9232b404dcde40b4600fb2ec14f37a793f6`:

1. Focused host test:
   `g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
   -Ifirmware/include firmware/tests/m7/test_m7p7f_ble_application_transport.cpp
   firmware/src/ble_application_transport.cpp firmware/src/application_request.cpp
   firmware/src/config_store.cpp firmware/src/config_format.cpp
   firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp` — **PASS**.
2. `firmware/src/ble_application_transport.cpp` additionally compiled clean
   under the pinned `-std=gnu++11` ARM-toolchain-compatible mode with the
   same warnings-as-errors + ASan/UBSan flags — **PASS**.
3. Full `bash firmware/tests/run_host_tests.sh` (includes the new
   M7P7F line, all prior M0-M7P7E regressions, all 8 startup scenarios):
   **PASS, exit 0**.
4. `pio run -d firmware -e rak4630`: **PASS**.
   - RAM: **22,124 / 248,832 bytes = 8.9%**
   - Flash: **226,292 / 815,104 bytes = 27.8%**
   - Delta versus the merged M7P7E production baseline (22,124 RAM /
     226,292 Flash): **+0 B RAM / +0 B Flash** — `ble_application_transport.cpp`
     compiles into the production object graph (confirmed present in the
     `check_exclusive_owner`/`check_application_ceiling` object list) but
     contributes nothing to the linked image because nothing in `main.cpp`
     references it; the linker's existing `--gc-sections` discipline
     discards it entirely, exactly as intended for a pre-wire slice.
   - `check_exclusive_owner`: **PASS**.
   - `check_application_ceiling`: **PASS**.
5. `pio run -d firmware -e rak4630_m7p7a_compile`: **PASS, unchanged** — RAM
   16,128 B, Flash 127,468 B (this target's explicit source whitelist does
   not include the new file at all).
6. No physical hardware test was performed or is required: this slice adds
   no Bluefruit GATT runtime, no advertising/admission change and no
   BLE-triggered application execution; there is nothing new to observe on
   a phone yet.

These PASS results apply to the pre-audit head `afd2e923...`. The independent
follow-up review below found session-provenance/ingress backpressure gaps and
changed code/tests to close them. The corrected head therefore requires fresh
owner-run focused/full host tests and the production PlatformIO build before
merge; the pre-audit PASS is not promoted to the corrected code.

## 13. Self-audit (section 18 of the task)

- Can peer-controlled bytes select `ApplicationRequester`? **No** —
  `ApplicationRequester::kBle` is a hard-coded literal at the single
  `service_.submit()` call site in `dispatchInbound()`; no frame field maps
  to it.
- Can one BLE session receive another session's response? **No** —
  `beginSession()` unconditionally clears outbound state and bumps the
  generation; `endSession()` is generation-gated (host test 14).
- Can BLE leave the global `ApplicationRequestService` slot wedged? **No** —
  the response is taken into the local buffer synchronously right after
  `submit()` returns `kAccepted`, and both `beginSession()`/`endSession()`
  defensively call `discardResponse(kBle)` (host tests 7-9, 12).
- Can a non-reading BLE client overwrite a pending response? **No** — stop-
  and-wait backpressure in `dispatchInbound()` drops new requests while
  `outbound_.pending` is true, before ever touching the global service
  (host test 9).
- Can BLE clear a USB response? **No** — `discardResponse(kBle)` is a no-op
  unless the pending response's requester is `kBle` (enforced inside
  `ApplicationRequestService` itself, from M7P7E); host tests 5-6 assert
  the USB response survives and remains USB-readable.
- Can malformed fragments trigger application/config/storage work? **No** —
  every validation failure returns before `service_.submit()` is ever
  called; `flash.program_calls`/`erase_calls` stay 0 throughout the
  malformed-input and flood tests.
- Does any callback-facing design imply flash/crypto/radio/application
  execution from BLE task context? **No** — `BleApplicationTransport` has
  no Bluefruit dependency at all and is documented as loop/task-driven
  only; it defines no callback.
- Did TLP v1 change? **No.**
- Did any persistent format change? **No** — no new storage region, no
  ConfigStore/SecurityStore/HistoryStore byte change.
- Did BLE admission/power behavior change? **No** — `ble_admission_policy.*`
  is untouched; `main.cpp`'s BLE section is untouched.
- Did this slice accidentally treat bond/connection/session as
  authorization? **No** — session generation is explicitly documented and
  implemented as transport correlation only; the only exposed operation is
  the section 6 pre-authorization allowlist, itself unchanged from M7P7D/E.

## 14. Independent follow-up audit and fixes

Independent review of `afd2e9232b404dcde40b4600fb2ec14f37a793f6` found:

- **MEDIUM:** only `endSession()` was generation-gated; delayed old-session
  ingress or indication-confirm events could otherwise affect a replacement
  session once M7P7G queues callback-derived events.
- **MEDIUM:** stop-and-wait was enforced only after complete reassembly in
  `dispatchInbound()`, allowing a peer to pre-stage a partial next request
  while the previous response still awaited confirmation.
- **LOW:** `session_generation_` could wrap through zero and there was no
  explicit active-session state.

Fixes on the same branch:

- generation is now required for inbound frame delivery, outbound peek and
  indication confirmation as well as disconnect cleanup;
- stale/inactive generations are pure no-ops before any current-session state
  is touched;
- stop-and-wait is enforced at ingress while an outbound response is pending,
  preventing partial-request pre-staging;
- session generations skip zero and an explicit bounded `session_active_`
  bit closes inactive-session event handling;
- focused host coverage now includes delayed old-session frame/peek/confirm/
  disconnect behavior and the partial-request pre-staging case.

**Post-fix owner revalidation is still pending** and must complete before merge.

## 15. What remains for M7P7G

M7P7F does not make BLE application runtime physically available. The next
slice must wire this tested contract to Bluefruit:

```text
callback/task
 -> fixed bounded ingress mailbox
 -> loop-owned BleApplicationTransport
 -> ApplicationRequestService
 -> local bounded response
 -> Bluefruit indication
```

and then physically validate with a phone: service discovery, request
write, response indication, connect/disconnect/reconnect, partial-fragment
disconnect, indication retry/non-reader behavior, no stale cross-session
response, and that tracker BLE admission behavior is still correct.
Commissioning/authentication and protected writes remain later, separately
gated focused work.
