# SF5K3R — Real HMAC Gateway ACK + durable flash chain (host-only)

Stacked on SF5K3Q. This is a **non-activated integration test**, not a
new trust anchor or RF implementation. It uses real production
`GatewayLocalCustodyOwner`, `GatewayCustodyAckSigner`,
`ProductCustodyStore`, frozen `custody_ack.cpp` and TLP v2 codec, with
an independent host-only OpenSSL primitive provider.

The end-to-end host fixture binds the exact PERIODIC 100-byte and EVENT
84-byte protected objects to the independently recorded 56-byte HMAC-ACK
oracles, after confirming durable flash commit + exact readback. Exercises
synchronous/asynchronous fake NOR, idempotent re-ACK, remount after a
simulated reboot and failed flash commit (no ACK).

Local isolation: actual store, owner and signer compiled with GCC and
ASan/UBSan, but a **test-only structural codec/ACK-parser stand-in** was
required because the full project source tree was unavailable. Those local
tests PASS. The checked-in runner instead compiles the **actual production
codec and ACK parser**, which is not yet executed or verified as a complete
repository test. Do not call this a full host/RAK4630 or hardware PASS.

**No Gateway authorization/key provisioning or enrollment lifecycle.**
The host test supplies a synthetic already-derived scoped HMAC key. No
live RF ACK receiver/sender, Edge proof, Relay reverse routing, or Tracker
release owner composition. Never deploy this as custody service yet.
