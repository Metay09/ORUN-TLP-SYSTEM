# M2 TLP POSITION packet

M2 POSITION is a fixed-length 34-byte private LoRa P2P payload. It is binary,
not JSON, and is serialized field by field rather than by sending a C++
structure.

All multi-byte fields use network byte order (unsigned or signed big-endian as
identified below).

| Offset | Length | Field | Type / units |
| ---: | ---: | --- | --- |
| 0 | 1 | `protocol_version` | `uint8`, `0x01` |
| 1 | 1 | `packet_type` | `uint8`, `0x02` (`POSITION`) |
| 2 | 8 | `source_device_id` | `uint64`, big-endian nRF52840 hardware ID |
| 10 | 4 | `sequence_number` | `uint32`, big-endian |
| 14 | 4 | `gnss_utc_epoch_seconds` | `uint32`, big-endian Unix UTC seconds; zero when UTC is not valid |
| 18 | 4 | `latitude_e7` | `int32`, big-endian; degrees × 10^7 |
| 22 | 4 | `longitude_e7` | `int32`, big-endian; degrees × 10^7 |
| 26 | 4 | `altitude_mm` | `int32`, big-endian; millimetres above ellipsoid |
| 30 | 2 | `hdop_x100` | `uint16`, big-endian; HDOP × 100 |
| 32 | 1 | `satellites` | `uint8`; satellites used in navigation solution |
| 33 | 1 | `flags` | `uint8` |

Total packet length: **34 bytes**.

## Flags

| Bit | Name | Meaning |
| ---: | --- | --- |
| 0 | `valid_fix` | u-blox `gnssFixOK` is set and fix type is 2D, 3D, or GNSS+dead-reckoning |
| 1 | `valid_gnss_utc_time` | u-blox NAV-PVT reports both valid UTC date and valid UTC time |
| 2 | `fix_3d` | u-blox fix type is 3D or GNSS+dead-reckoning |
| 3-7 | reserved | Must be zero; receivers reject packets with these bits set |

## Fresh-fix rule

POSITION is only generated from a newly received UBX-NAV-PVT message. The
GnssManager accepts it only when `gnssFixOK` is set, its navigation fix type is
2D/3D/GNSS+dead-reckoning, the module has not marked latitude/longitude/height
invalid, and the latitude and longitude are within their legal ranges.

The associated UBX-NAV-DOP message must carry the same `iTOW` navigation epoch;
its `hDOP` is sent as `hdop_x100`. This avoids inventing an HDOP value or
pairing position and quality values from different epochs. A central 60-second
M2 test interval selects the first qualifying fresh fix after the interval;
cached coordinates are never sent simply because the interval elapsed.

PVT and DOP callbacks may arrive in either order. The manager keeps the latest
candidate PVT and DOP separately and promotes a fix only after their `iTOW`
values match. The promoted fix has a separate single-use buffer: it is consumed
by the transmit path once and cannot be overwritten by later callbacks. A
matching `iTOW` already promoted during the current runtime is not promoted
again until another PVT epoch is observed; this is an equality check only, not
a monotonic `iTOW` comparison. This avoids treating the GPS-week-wrapping iTOW
as an ever-increasing counter.

UTC epoch is produced by the SparkFun library's `getUnixEpoch()` implementation
only when NAV-PVT reports both UTC date and time valid. It is otherwise zero
and flag bit 1 is clear.

`altitude_mm` is NAV-PVT `height`: signed millimetres above the WGS84 ellipsoid
(`hMSL`, the mean-sea-level alternative, is not used). NAV-DOP `hDOP` is already
stored by u-blox as HDOP × 0.01, so it is copied directly to `hdop_x100` with no
additional scaling.
