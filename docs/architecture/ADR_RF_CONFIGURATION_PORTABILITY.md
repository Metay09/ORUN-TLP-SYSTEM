# ADR — RF configuration semantics and radio-platform portability

Status: **OWNER-APPROVED ARCHITECTURE DIRECTION — 2026-09-23**.

This record defines how ORUN may make RF settings configurable without coupling the
product model, persistent configuration or future TLP v2 command semantics to the
current RAK4630/RAK4631 + SX1262 implementation.

It does not add a second hardware platform, a generic radio HAL, a new RF profile,
a new persistent config field or a runtime behavior change by itself.

## 1. Reference platform versus product semantics

The owned reference platform remains RAK4630/RAK4631 with SX1262 and
SX126x-Arduino 2.0.32. It is the platform on which current behavior is implemented,
tested and physically validated.

Future ORUN hardware may use another board, MCU or LoRa transceiver. That is a
portability constraint, not authorization to build speculative drivers/frameworks now.

One product codebase means shared product/protocol/configuration/application semantics.
A future second platform may legitimately build a different board-specific binary.

## 2. Persistent/public RF configuration uses physical meaning

Persistent configuration, BLE/USB application schemas and future TLP v2 protected
configuration commands must use hardware-neutral physical/LoRa semantics.

Examples of acceptable semantic values when those fields are implemented:

```text
tx_power_dbm
frequency_hz
bandwidth_hz
spreading_factor
coding_rate (semantic LoRa rate, e.g. 4/5)
preamble_symbols
```

Do **not** persist or expose SX126x-Arduino enum/index/register values such as
"bandwidth 0 means 125 kHz" or "coding-rate 1 means 4/5". Those encodings belong
inside the current radio adapter/application boundary.

The current private-LoRa sync setting also has driver/Semtech representation
semantics. Do not assume raw `0x1424` is a universal cross-radio product field.
If sync/network-domain configuration becomes public, define its portable semantic
contract first, then translate it for each supported radio.

## 3. Hardware capability is not a universal ORUN limit

The current SX1262/RAK reference radio's supported TX-power range is a capability
fact of that platform. A value such as +22 dBm must never become an eternal ORUN
protocol/configuration maximum merely because the first radio uses it.

Likewise, the current driver's 255-byte copied RX ceiling is a local implementation
bound, not a TLP v2 MTU promise.

A future radio implementation supplies its own supported ranges/features. Product
configuration must not encode current-driver limits as wire-format meaning.

Regulatory/install limits are another independent policy input:

```text
hardware capability
!= regional RF rule
!= antenna/cable/install constraint
!= requested RF configuration
!= effective/applied RF state
```

Do not hard-code a legal conclusion from the transceiver's data-sheet maximum.

## 4. Requested, effective and applied RF state remain distinct

The intended flow is:

```text
profile / user / authorized command
              |
              v
      Requested RF config
              |
              v
 capability + regulatory/power policy validation
              |
              v
      Effective RF config
              |
              v
    single RadioManager apply path
              |
              v
      concrete radio driver
```

For the first TX-power configuration slice, an unsupported/out-of-policy candidate
must be rejected without changing flash, requested state or the previously applied
working radio configuration. Do not silently clamp and then report that the requested
value was applied.

If a later field needs a different "valid intent but currently blocked" policy, that
must be explicit and reasoned; it must not silently rewrite requested intent.

## 5. One application path for boot and runtime reconfiguration

Do not create separate RF programming logic for:

- boot recovery from ConfigStore;
- USB configuration;
- BLE configuration;
- future TLP v2 authenticated remote configuration.

All accepted changes must converge on the same owner and the same effective-config
application path.

A runtime change must preserve the existing single-radio-owner, driver-gate,
callback-handoff, quiescence, TX-terminal and RX-restore invariants. "Not currently
transmitting" alone is not proof that a driver can be safely reconfigured; the
pinned driver behavior must be audited for each affected operation.

## 6. RX configuration is three separate concerns

There is no TX-like "RX power dBm" setting. Keep these separate:

1. **LoRa modem compatibility** — frequency, SF, bandwidth, coding rate, preamble,
   sync/network-domain semantics.
2. **RX availability/power policy** — continuous, windowed, asleep/rendezvous.
3. **Receiver gain/sensitivity policy** — only when supported and justified by a
   real radio/platform (for SX1262, normal versus boosted gain is a hardware-specific
   capability/power tradeoff).

Do not put RX availability policy into the RF modem profile. Relay forwarding and
gateway availability commitments continue to own continuous-listen requirements.

## 7. Current concrete boundaries and future extraction trigger

Today these implementation details are intentionally concrete:

- `RadioManager` calls SX126x-Arduino directly;
- `radio_config.h` contains current profile defaults plus current driver encodings;
- `patch_radio.py` and `radio_driver_gate` implement the audited SX126x/nRF52
  concurrency path;
- `RakDeviceIdentityProvider`, Nordic flash backends, Bluefruit glue and
  `SensorPowerManager` are current-board adapters.

This is not a defect requiring an immediate generic HAL.

When a **real second radio/platform is selected and available**, extract only the
small demonstrated seam required to preserve:

- bounded TX/RX frame ownership;
- normalized RSSI dBm / SNR observations;
- safe TX completion/timeout semantics;
- quiesce/reconfigure/restart behavior;
- radio capability reporting;
- semantic RF-config translation.

Do not invent `IRadio`, plugin factories, generic driver registries or unused
platform implementations before that trigger.

## 8. Protocol and remote configuration boundary

TLP v2 security/application semantics must not depend on SX1262 register values.
A future protected RF-config command carries semantic requested values and the
target device performs capability/policy validation.

Gateway receipt or RF TX completion is not proof of configuration success. The
target's authenticated command result must distinguish rejection, acceptance and
applied/effective state as the command architecture is implemented.

TLP v1 remains unchanged by this ADR.

## 9. Near-term implementation rule

The next RF-config slice should remain deliberately narrow:

1. make `tx_power_dbm` a versioned persistent semantic config value;
2. keep the current 14 dBm default so existing behavior does not change merely
   because persistence is added;
3. apply current SX1262 capability/policy validation outside the on-flash field's
   semantic meaning;
4. use one boot/runtime application path;
5. leave frequency/SF/BW/CR/preamble/sync and RX-gain runtime behavior unchanged
   until separately justified.

Physical range/current testing is valuable optimization evidence but is not required
to define the semantic/configuration boundary. Unmeasured behavior must not be
reported as physically validated.
