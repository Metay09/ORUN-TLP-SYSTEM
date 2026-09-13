#pragma once
#include <stddef.h>
#include <stdint.h>
#include "storage_config.h"
#include "tlp_position_packet.h"

namespace orun_tlp::journal_format {
constexpr uint32_t kPageMagic = 0x4F524A34;  // ORJ4
constexpr uint8_t kVersion = 2;
constexpr uint32_t kCommit = 0;
constexpr uint32_t kStaticHeaderSize = 64;
constexpr uint32_t kSequenceSlotSize = 16;
constexpr uint32_t kStateSlotSize = 32;
uint32_t crc32(const uint8_t* data, size_t size);
void put16(uint8_t* data, uint16_t value);
void put32(uint8_t* data, uint32_t value);
void put64(uint8_t* data, uint64_t value);
uint16_t get16(const uint8_t* data);
uint32_t get32(const uint8_t* data);
uint64_t get64(const uint8_t* data);
bool erased(const uint8_t* data, size_t size);
bool looksLikeJournalHeader(const uint8_t* bytes);
struct Record {
  uint64_t identity = 0;
  uint8_t packet[tlp::kPositionPacketSize]{};
};
struct State { uint64_t generation = 0, delivered_through = 0, replay_cursor = 0; };
bool validPacket(const uint8_t* packet, uint64_t identity, uint64_t device);
void encodeRecord(const Record& record, uint8_t* bytes);
bool decodeRecord(const uint8_t* bytes, uint64_t device, Record& record);
void encodePage(uint64_t generation, uint64_t device, uint8_t* bytes);
bool decodePage(const uint8_t* bytes, uint64_t device, uint64_t& generation);
void encodeSequenceEnd(uint64_t sequence_end, uint8_t* bytes);
bool decodeSequenceEnd(const uint8_t* bytes, uint64_t& sequence_end);
void encodeState(const State& state, uint8_t* bytes);
bool decodeState(const uint8_t* bytes, State& state);
}  // namespace orun_tlp::journal_format
