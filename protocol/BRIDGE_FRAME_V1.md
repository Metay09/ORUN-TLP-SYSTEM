# Host bridge line v1

A node that accepts an application POSITION prints one machine-readable USB
serial line for it. An attached host (PC, phone, Edge) reads these lines and
carries the observations onward. Today the only node that accepts application
POSITION is the legacy `BASE` receiver.

Before this line existed the receiver printed only source, sequence and link
quality (`BASE RX ...`), so the received coordinates never left the radio.
The human-readable `BASE RX ...` lines are unchanged and still printed.

## Boundary

- Development host output for frozen TLP v1 traffic.
- **Not** durable custody, **not** a delivery ACK, **not** authenticated, and
  not the future Gateway/Edge custody contract (SF5G/H). A printed line proves
  nothing about a host having received it.
- No LoRa byte, flash content or radio behavior changes. Output only.
- The node is a transparent carrier: it prints the original packet bytes and
  does not reinterpret them. The host decodes
  [`M2_POSITION_PACKET.md`](M2_POSITION_PACKET.md).

## Line format

One ASCII line, fields separated by single spaces, in exactly this order,
terminated by the serial line ending (CR LF). Hex is uppercase. Numbers are decimal
without padding.

Direct reception:

```text
BRIDGE v=1 kind=POSITION n=<n> up=<ms> node=<id> dup=<0|1> path=DIRECT rssi=<dBm> snr=<dB> raw=<68 hex> crc=<4 hex>
```

Reception through one relay:

```text
BRIDGE v=1 kind=POSITION n=<n> up=<ms> node=<id> dup=<0|1> path=RELAY relay=<id> in_rssi=<dBm> in_snr=<dB> rssi=<dBm> snr=<dB> raw=<68 hex> crc=<4 hex>
```

| Field | Meaning |
| --- | --- |
| `v` | Line format version, `1`. A host must ignore lines with an unknown `v` or `kind`. |
| `kind` | `POSITION`. Later kinds get their own name; they never reuse this one. |
| `n` | Per-boot line counter; the first line after boot is `1`. A gap means a line was lost on the serial link; a restart at `1` means the node rebooted. Not a dedupe or delivery identity. |
| `up` | Node monotonic milliseconds since boot when the line was printed; wraps at 2^32. The node has no trusted wall clock: the host stamps its own receipt time. |
| `node` | 64-bit device ID of the node that received the packet and printed the line, 16 hex digits. |
| `dup` | `1` when the node's bounded RAM dedupe had already seen this (source, sequence), else `0`. Duplicates are still printed because another path is useful evidence. Long-term dedupe on (source device ID, sequence) belongs to the host/backend. |
| `path` | `DIRECT` or `RELAY`. |
| `relay` | Relay path only: 64-bit device ID of the forwarding relay. |
| `in_rssi`, `in_snr` | Relay path only: link quality source -> relay, as reported by the relay. |
| `rssi`, `snr` | Link quality of the last hop into this node. |
| `raw` | The original 34-byte TLP v1 POSITION exactly as its source transmitted it (for the relay path: the inner original packet), 68 hex digits. |
| `crc` | CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, no reflection, no final XOR; check value of `123456789` is `29B1`) over every character from the leading `B` through the last `raw` digit. A host must drop a line whose checksum does not match. |

The longest possible line is 243 characters.

The observation time of the position is the `gnss_utc_epoch_seconds` inside
`raw` (zero when the source had no valid UTC). A host must keep observation
time, node `up` and its own receipt time as three separate facts and must not
present an old observation as live.

## Reference lines

These exact lines are asserted by
`firmware/tests/m5/test_bridge_frame.cpp`. A host decoder must accept them.

```text
BRIDGE v=1 kind=POSITION n=1 up=123456 node=0102030405060708 dup=0 path=DIRECT rssi=-82 snr=6 raw=010289ABCDEF01234567000000286553F10018701A8011490C800000303900AF0907 crc=8E07
```

`raw` decodes to source `89ABCDEF01234567`, sequence `40`, UTC `1700000000`,
latitude `41.0000000`, longitude `29.0000000`, altitude `12.345 m`, HDOP
`1.75`, 9 satellites, flags `0x07`.

```text
BRIDGE v=1 kind=POSITION n=4294967295 up=4294967295 node=0123456789ABCDEF dup=1 path=RELAY relay=FEDCBA9876543210 in_rssi=-32768 in_snr=-128 rssi=-32768 snr=-128 raw=0102FFFFFFFFFFFFFFFFFFFFFFFF00000000CA5B170094B62E0080000000FFFFFF00 crc=792F
```

The second line is the format's widest case; it exercises field widths, not a
position a real node would send.

## Not covered yet

- Opaque secure objects (`HISTORY_SECURE`, future `PRODUCT_SECURE`): a later
  `kind`, carried the same transparent way.
- Host -> node direction (commands, custody ACK): separate reviewed contracts.
- BLE transport of the same observations.
