#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tlp_position_packet.h"

// Host bridge line v1.
//
// A node that accepts an application POSITION (today: the legacy BASE
// receiver) prints exactly one machine-readable USB serial line for it, so an
// attached host (PC, phone, Edge) can carry the observation onward. Before
// this line existed the receiver printed only source/sequence/link quality,
// so the received coordinates never left the radio.
//
// The line carries the ORIGINAL 34-byte TLP v1 POSITION untouched plus what
// only the receiver knows (path, link quality, its own identity). The host
// decodes the packet; the node stays a transparent carrier.
//
// Boundary: this is a development host output for frozen TLP v1 traffic. It is
// NOT durable custody, NOT a delivery ACK, NOT authenticated and NOT the
// future Gateway/Edge custody contract. Printing a line proves nothing about
// a host having received it. No RF byte, flash or radio behavior changes.
//
// Contract: protocol/BRIDGE_FRAME_V1.md.
namespace orun_tlp::bridge_frame {

constexpr uint8_t kVersion = 1;

// Longest possible line (relay path, extreme values) is 243 characters; the
// buffer also holds the NUL terminator.
constexpr size_t kMaxLineSize = 256;

struct ReceivedPosition {
  // Per-boot line counter, first line is 1. Lets a host see dropped lines
  // and node reboots. Not a delivery or dedupe identity.
  uint32_t line_number = 0;
  // Receiver monotonic milliseconds since boot (wraps at 2^32). The receiver
  // has no trusted wall clock; the host stamps receipt time itself.
  uint32_t uptime_ms = 0;
  uint64_t receiver_device_id = 0;
  // True when the receiver's bounded RAM dedupe already saw this
  // (source, sequence). Still printed: another path is useful evidence and
  // long-term dedupe belongs to the host/backend.
  bool duplicate = false;
  bool relayed = false;
  uint64_t relay_device_id = 0;   // relayed only
  int16_t ingress_rssi_dbm = 0;   // relayed only: source -> relay
  int8_t ingress_snr_db = 0;      // relayed only: source -> relay
  int16_t link_rssi_dbm = 0;      // last hop into this receiver
  int8_t link_snr_db = 0;         // last hop into this receiver
  // Exact original POSITION bytes as transmitted by their source.
  uint8_t packet[tlp::kPositionPacketSize]{};
};

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final XOR.
uint16_t crc16(const char* text, size_t size);

// Writes one NUL-terminated line WITHOUT a trailing newline and returns its
// length. Returns 0 and leaves an empty string when output cannot hold the
// complete line; a truncated line is never produced.
size_t formatPositionLine(const ReceivedPosition& received, char* output,
                          size_t output_size);

}  // namespace orun_tlp::bridge_frame
