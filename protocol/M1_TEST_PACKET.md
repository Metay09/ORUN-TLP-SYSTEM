# M1 TLP TEST packet

This is the M1 private LoRa P2P test packet. It is a fixed 18-byte binary
payload; it is not a C/C++ structure and it is not JSON.

All multi-byte integer fields use unsigned big-endian (network) byte order.

| Offset | Length | Field | Encoding/value |
| ---: | ---: | --- | --- |
| 0 | 1 | `protocol_version` | `0x01` |
| 1 | 1 | `packet_type` | `0x01` (`TEST`) |
| 2 | 8 | `source_device_id` | nRF52840 hardware ID, unsigned 64-bit big-endian |
| 10 | 4 | `sequence_number` | unsigned 32-bit big-endian |
| 14 | 4 | `uptime_ms` | unsigned 32-bit big-endian `millis()` value |

Total packet length: **18 bytes**.

Receivers reject a payload unless its length is exactly 18 bytes and both the
protocol version and packet type match the values above. LoRa PHY CRC is
enabled independently of this payload format.

## M1 LoRa PHY profile

The M1 TEST payload uses the following central radio configuration: 869.525
MHz, 14 dBm configured conducted TX power, SF11, 125 kHz bandwidth, 4/5 coding
rate, 8-symbol preamble, explicit header, PHY CRC enabled, and non-inverted IQ.
The private LoRa sync word is explicitly set to `0x1424` using the
SX126x-Arduino `SetCustomSyncWord()` API and read back with `GetSyncWord()` for
the boot RF-profile log. `0x1424` is the library/Semtech private LoRa value;
it is not a secret or authentication mechanism.

For SF11/BW125, SX126x-Arduino 2.0.32 automatically enables Low Data Rate
Optimization in both its TX and RX configuration paths. M1 intentionally does
not call its manual `EnforceLowDRopt()` override.

`source_device_id` is assembled from the eight bytes returned by
`BoardGetUniqueId()` in index order (`id[0]` as the most significant byte).
The SX126x-Arduino nRF52 implementation reads the nRF52840 factory device-ID
register pair; the firmware does not assign a shared, fixed device ID.
