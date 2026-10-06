# M4P5C — tracker History store-forward runtime activation

Status: **ACTIVE IMPLEMENTATION / VALIDATION PENDING**

Baseline:

```text
main@678267d2d6f406dd803f7c853039e90295b94737
```

Branch:

```text
feat/m4p5c-history-receipt-composition
```

## Goal

Stop accumulating transport-neutral pieces without using them on the device.

This slice activates the already-reviewed History store-forward foundations in
the normal RAK4630 firmware while keeping frozen wire, flash formats and
security ownership unchanged.

Normal production now composes:

```text
HistoryStore retained record
 -> HistorySecureCrypto DEVICE_D2A protection
 -> RadioManager HISTORY_SECURE TX
 -> existing post-TX TRACKER RX window

RadioManager raw HISTORY_SECURE RX
 -> bounded one-frame loop handoff
 -> HistorySecureCrypto BACKEND_A2D open
 -> opaque AuthenticatedBackendDurableReceipt
 -> SecurityStore A2D replay admission
 -> accepted == true
 -> M4P5A RAM delivery watermark
```

## Runtime ownership

`RadioManager` owns only SX1262 transport and one bounded raw-frame handoff.
It does not own crypto, replay admission or History delivery.

`HistorySecureCrypto` keeps the reviewed root-credential/KDF/AES-CCM
boundary.

`HistoryReceiptAdmissionCoordinator` remains the sole consumer of the
SecurityStore A2D replay result while a receipt is pending.

`HistoryDeliveryCoordinator` remains the bounded RAM delivery owner.

`HistoryStore` remains persistence authority.

Crypto never runs in an RF callback or while RadioManager holds its driver
gate. RadioManager copies the complete received HISTORY_SECURE frame in loop
context; the composition root decrypts it only after `RadioManager::update()`
returns.

## Initial replay policy

This is deliberately conservative until real collision-domain measurements
exist.

- current/live PositionFlow gets first TX opportunity;
- an inbound backend receipt gets priority over background replay;
- relay-forwarding-enabled nodes do not originate backlog replay;
- selection starts after `HistoryStore::acknowledgedThrough()`, not merely
  durable `delivered_through`;
- when no accelerated authenticated-contact policy exists, at most one oldest
  unacknowledged record is probed every 15 minutes;
- startup phase is deterministically staggered by device ID over an additional
  0..5 minute window;
- `path_flags=0`: this activation is direct only; secure relay forwarding is
  not enabled;
- no persistent replay cursor is used.

A failed TX may burn a SecurityStore D2A counter. That is safe; the logical
History record stays undelivered and is retried later with a fresh counter.

## Receipt behavior

A received frame is retained in the one-frame radio handoff slot while
SecurityStore/History is transiently busy.

Unprovisioned/fault security cannot authenticate a receipt, so such a frame is
dropped rather than clogging the handoff slot indefinitely.

Authenticated application delivery still follows the strict order:

```text
AEAD success
 -> exact opaque credential/epoch/counter tuple
 -> durable A2D replay admission
 -> accepted=true
 -> M4P5A delivery
```

No History delivery mutation happens before replay acceptance.

## Persistence boundary

This slice does **not** add a new durable checkpoint policy.

The current authenticated RAM watermark is honored during the boot, so already
accepted records are not continuously retransmitted.

A reboot before a later coarse checkpoint may replay already-delivered records.
That is safe duplicate work and remains preferable to metadata-driven page
rotation.

The existing safe `checkpointAcknowledgedDelivery()` foundation is unchanged
and no per-record `markDeliveredThrough()` call is introduced here.

## Fail-closed activation gates

Normal production runtime is linked and called on every boot, but secure replay
requires:

- HistoryStore ready;
- SecurityStore ready and PROVISIONED;
- Bluefruit/CC310 lifecycle ready;
- radio idle;
- no higher-priority live PositionFlow work;
- relay forwarding disabled.

A blank development device therefore reports the runtime as active but
security unavailable; it does not transmit unauthenticated History.

## Compatibility

Unchanged:

- TLP v1 packet bytes and existing POSITION/RELAY behavior;
- frozen M4P4 HISTORY_SECURE wire bytes;
- M7P6I KDF/nonce/AES-CCM contract;
- SecurityStore format/layout;
- HistoryStore format/layout;
- GNSS policy;
- BLE application policy;
- existing relay forwarding semantics.

## Not implemented here

- gateway/backend ingestion and receipt generation;
- secure HISTORY_SECURE relay forwarding;
- contact-aware fast backlog drain;
- durable coarse-checkpoint cadence;
- mobile/backend UI.

## Validation process

This runtime integration does not require another independent audit round.

Required before merge:

1. focused M4P5C vector + active-runtime source-contract gates;
2. aggregate host regression;
3. normal RAK4630 production build;
4. M4P5C target build;
5. physical normal-firmware boot check showing the store-forward runtime state;
6. physical scoped target KAT for the crypto/admission chain if needed;
7. one focused code review of runtime ordering, radio ownership and power/
   airtime behavior.

Host/build PASS is not physical RF or end-to-end backend proof.
