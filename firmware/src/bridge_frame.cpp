#include "bridge_frame.h"

namespace orun_tlp::bridge_frame {
namespace {

// Bounded text builder. Deliberately no printf: the target's nano printf has
// no long-long support and the line must be byte-identical on host and node.
class LineWriter {
 public:
  LineWriter(char* output, size_t size) : output_(output), size_(size) {}

  void put(char value) {
    // Always keep one byte for the NUL terminator.
    if (length_ + 1 < size_) {
      output_[length_++] = value;
    } else {
      overflowed_ = true;
    }
  }

  void text(const char* value) {
    while (*value != '\0') put(*value++);
  }

  void decimal(uint32_t value) {
    char digits[10];
    uint8_t count = 0;
    do {
      digits[count++] = static_cast<char>('0' + value % 10U);
      value /= 10U;
    } while (value != 0U);
    while (count != 0U) put(digits[--count]);
  }

  void signedDecimal(int32_t value) {
    if (value < 0) {
      put('-');
      decimal(uint32_t{0} - static_cast<uint32_t>(value));
    } else {
      decimal(static_cast<uint32_t>(value));
    }
  }

  void hex(uint64_t value, uint8_t digits) {
    static const char kDigits[] = "0123456789ABCDEF";
    while (digits != 0U) {
      --digits;
      put(kDigits[(value >> (4U * digits)) & 0xFU]);
    }
  }

  size_t length() const { return length_; }
  bool overflowed() const { return overflowed_; }

 private:
  char* output_;
  size_t size_;
  size_t length_ = 0;
  bool overflowed_ = false;
};

}  // namespace

uint16_t crc16(const char* text, size_t size) {
  uint16_t crc = 0xFFFFU;
  for (size_t index = 0; index < size; ++index) {
    crc = static_cast<uint16_t>(
        crc ^ (static_cast<uint16_t>(static_cast<uint8_t>(text[index])) << 8));
    for (uint8_t bit = 0; bit < 8U; ++bit) {
      crc = static_cast<uint16_t>((crc & 0x8000U) != 0U ? (crc << 1) ^ 0x1021U
                                                         : crc << 1);
    }
  }
  return crc;
}

size_t formatPositionLine(const ReceivedPosition& received, char* output,
                          size_t output_size) {
  if (output == nullptr || output_size == 0U) return 0;

  LineWriter line(output, output_size);
  line.text("BRIDGE v=");
  line.decimal(kVersion);
  line.text(" kind=POSITION n=");
  line.decimal(received.line_number);
  line.text(" up=");
  line.decimal(received.uptime_ms);
  line.text(" node=");
  line.hex(received.receiver_device_id, 16);
  line.text(" dup=");
  line.put(received.duplicate ? '1' : '0');
  if (received.relayed) {
    line.text(" path=RELAY relay=");
    line.hex(received.relay_device_id, 16);
    line.text(" in_rssi=");
    line.signedDecimal(received.ingress_rssi_dbm);
    line.text(" in_snr=");
    line.signedDecimal(received.ingress_snr_db);
  } else {
    line.text(" path=DIRECT");
  }
  line.text(" rssi=");
  line.signedDecimal(received.link_rssi_dbm);
  line.text(" snr=");
  line.signedDecimal(received.link_snr_db);
  line.text(" raw=");
  for (size_t index = 0; index < sizeof(received.packet); ++index) {
    line.hex(received.packet[index], 2);
  }

  // The checksum covers every character printed so far, from the leading 'B'
  // through the last raw hex digit.
  const uint16_t checksum = crc16(output, line.length());
  line.text(" crc=");
  line.hex(checksum, 4);

  if (line.overflowed()) {
    output[0] = '\0';
    return 0;
  }
  output[line.length()] = '\0';
  return line.length();
}

}  // namespace orun_tlp::bridge_frame
