# M5 TLP RELAY_FORWARD packet

M5 adds protocol-version `0x01`, packet-type `0x03` (`RELAY_FORWARD`). It
encapsulates one exact M2 POSITION packet without assigning a new TLP sequence
to the forwarded data. All multi-byte values use network byte order.

| Offset | Length | Field | Encoding / unit |
| ---: | ---: | --- | --- |
| 0 | 1 | `protocol_version` | `uint8`, `0x01` |
| 1 | 1 | `packet_type` | `uint8`, `0x03` |
| 2 | 8 | `relay_device_id` | `uint64`, big-endian relay hardware ID |
| 10 | 1 | `hop_count` | `uint8`, exactly `1` |
| 11 | 1 | `original_length` | `uint8`, currently exactly `34` |
| 12 | 2 | `ingress_rssi_dbm` | signed `int16`, two's-complement big-endian, dBm |
| 14 | 1 | `ingress_snr_db` | signed `int8`, two's-complement, integer dB |
| 15 | 34 | `original_packet` | exact original TLP POSITION bytes |

Current and maximum accepted M5 length: **49 bytes**. The length field keeps
the envelope extensible, but M5 intentionally accepts only a 34-byte POSITION.
TEST packets are not relayed. The envelope has no new application sequence:
the inner `(source_device_id, sequence_number)` remains the identity used by
RELAY and BASE dedupe.

## RF metric source

SX126x-Arduino 2.0.32 declares `RxDone(..., int16_t rssi, int8_t snr)` with
RSSI in dBm and SNR in dB. Its SX126x driver calculates packet RSSI as the
negative raw value divided by two and rounds the radio's quarter-dB SNR to the
nearest integer before invoking the callback. M5 stores those callback values
unchanged: no rescaling, offset or normalization is applied. For a relayed
observation, these fields describe TRACKER→RELAY; BASE obtains RELAY→BASE RSSI
and SNR from its own `RxDone` callback.

## Validation

A decoder rejects the packet unless all of these hold:

- outer buffer is at least 15 bytes before any variable field is read;
- outer version is `0x01` and type is `0x03`;
- hop count is exactly one;
- original length is exactly 34 and total length is exactly `15 + 34`;
- inner version is `0x01`, inner type is POSITION `0x02`, and its exact length,
  coordinate ranges and reserved flag bits pass the existing M2 decoder;
- an inner type `0x03` is explicitly rejected as nested relay traffic.

RELAY nodes never forward any received RELAY_FORWARD, valid or malformed.
TRACKER nodes never forward traffic. Thus the only supported topology is one
hop: TRACKER→RELAY→BASE. The LoRa PHY CRC remains enabled, but this envelope
adds no authentication or cryptographic integrity.
