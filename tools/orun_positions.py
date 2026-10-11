#!/usr/bin/env python3
"""Readable positions from a receiver's USB log.

The receiver (legacy BASE) prints one machine line per received POSITION:
protocol/BRIDGE_FRAME_V1.md ("BRIDGE v=1 kind=POSITION ... raw=<68 hex>
crc=<4 hex>"). The coordinates are inside `raw`, the original 34-byte TLP v1
POSITION (protocol/M2_POSITION_PACKET.md). This host tool checks each line's
CRC, decodes the packet and prints one readable line with a map link. It can
also write a KML track for Google Earth / Google My Maps.

  python3 tools/orun_positions.py ~/alici.log
  python3 tools/orun_positions.py ~/alici.log --kml ~/iz.kml
  tail -f ~/alici.log | python3 tools/orun_positions.py

Time shown is the GNSS observation time inside the packet, in this computer's
local time with its UTC offset, not the time the line was read: an old
position is never shown as live. Repeated copies of the same (source,
sequence) are skipped unless --all is given.
"""

import argparse
import datetime
import sys

PACKET_SIZE = 34
FLAG_VALID_FIX = 0x01
FLAG_VALID_UTC = 0x02
FLAG_3D_FIX = 0x04
MARKER = "BRIDGE v=1 kind=POSITION "


def crc16(text):
    """CRC-16/CCITT-FALSE over ASCII text."""
    crc = 0xFFFF
    for byte in text.encode("ascii"):
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def signed32(value):
    return value - (1 << 32) if value & 0x80000000 else value


def parse_line(line):
    """Returns a dict for a valid bridge POSITION line, else None."""
    start = line.find(MARKER)
    if start < 0:
        return None
    body = line[start:].strip()
    crc_at = body.rfind(" crc=")
    if crc_at < 0:
        return None
    try:
        if int(body[crc_at + 5:crc_at + 9], 16) != crc16(body[:crc_at]):
            return None
    except ValueError:
        return None
    fields = {}
    for item in body[:crc_at].split(" ")[3:]:
        key, sep, value = item.partition("=")
        if not sep:
            return None
        fields[key] = value
    raw = fields.get("raw", "")
    if len(raw) != 2 * PACKET_SIZE:
        return None
    try:
        packet = bytes.fromhex(raw)
    except ValueError:
        return None
    if packet[0] != 0x01 or packet[1] != 0x02:
        return None
    u32 = lambda offset: int.from_bytes(packet[offset:offset + 4], "big")
    return {
        "received": line[:start].strip(),
        "duplicate": fields.get("dup") == "1",
        "path": fields.get("path", "?"),
        "rssi": fields.get("rssi", "?"),
        "snr": fields.get("snr", "?"),
        "source": packet[2:10].hex().upper(),
        "sequence": u32(10),
        "utc": u32(14),
        "lat_e7": signed32(u32(18)),
        "lon_e7": signed32(u32(22)),
        "altitude_mm": signed32(u32(26)),
        "hdop_x100": int.from_bytes(packet[30:32], "big"),
        "satellites": packet[32],
        "flags": packet[33],
    }


def degrees(value_e7):
    sign = "-" if value_e7 < 0 else ""
    value = abs(value_e7)
    return "%s%d.%07d" % (sign, value // 10000000, value % 10000000)


def utc_text(position):
    if not position["flags"] & FLAG_VALID_UTC or position["utc"] == 0:
        return "saat yok"
    # Shown in this computer's local time with its UTC offset (Turkey: +0300).
    moment = datetime.datetime.fromtimestamp(position["utc"],
                                             datetime.timezone.utc).astimezone()
    return moment.strftime("%Y-%m-%d %H:%M:%S %z")


def readable(position):
    head = "%s  tasma=%s sira=%d" % (utc_text(position), position["source"],
                                     position["sequence"])
    link = "sinyal=%s dBm snr=%s %s" % (position["rssi"], position["snr"],
                                        position["path"])
    if position["duplicate"]:
        link += " TEKRAR"
    if not position["flags"] & FLAG_VALID_FIX:
        return "%s  konum yok  %s" % (head, link)
    lat = degrees(position["lat_e7"])
    lon = degrees(position["lon_e7"])
    hdop = position["hdop_x100"]
    return "%s  %s,%s  uydu=%d hdop=%d.%02d  %s  https://maps.google.com/?q=%s,%s" % (
        head, lat, lon, position["satellites"], hdop // 100, hdop % 100, link,
        lat, lon)


def kml(positions):
    fixes = [p for p in positions if p["flags"] & FLAG_VALID_FIX]
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<kml xmlns="http://www.opengis.net/kml/2.2"><Document>',
           "<name>ORUN iz</name>"]
    for p in fixes:
        out.append("<Placemark><name>%s</name><description>tasma %s sira %d"
                   "</description><Point><coordinates>%s,%s</coordinates>"
                   "</Point></Placemark>" % (
                       utc_text(p), p["source"], p["sequence"],
                       degrees(p["lon_e7"]), degrees(p["lat_e7"])))
    if len(fixes) > 1:
        coordinates = " ".join("%s,%s" % (degrees(p["lon_e7"]),
                                          degrees(p["lat_e7"])) for p in fixes)
        out.append("<Placemark><name>iz</name><LineString><coordinates>%s"
                   "</coordinates></LineString></Placemark>" % coordinates)
    out.append("</Document></kml>")
    return "\n".join(out) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("logs", nargs="*", help="log files (default: stdin)")
    parser.add_argument("--kml", help="also write the track to this KML file")
    parser.add_argument("--all", action="store_true",
                        help="also show repeated copies of the same packet")
    args = parser.parse_args(argv)

    seen = set()
    kept = []
    streams = [open(name, errors="replace") for name in args.logs] or [sys.stdin]
    for stream in streams:
        for line in stream:
            position = parse_line(line)
            if position is None:
                continue
            key = (position["source"], position["sequence"])
            if key in seen and not args.all:
                continue
            seen.add(key)
            kept.append(position)
            print(readable(position), flush=True)
    if args.kml:
        with open(args.kml, "w") as handle:
            handle.write(kml(kept))
        print("KML: %s (%d konum)" % (args.kml, sum(
            1 for p in kept if p["flags"] & FLAG_VALID_FIX)), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
