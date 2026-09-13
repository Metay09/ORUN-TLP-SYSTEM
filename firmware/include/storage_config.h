#pragma once
#include <stdint.h>

namespace orun_tlp::storage_config {
// This is the core InternalFS partition, exclusively ORUN-owned while this
// backend is linked. It is not spare application flash.
constexpr uint32_t kBaseAddress = 0xED000;
constexpr uint32_t kPageSize = 4096;
constexpr uint32_t kPageCount = 7;
constexpr uint32_t kRegionSize = kPageSize * kPageCount;
constexpr uint32_t kPageHeaderSize = 352;
constexpr uint32_t kRecordSize = 36;
constexpr uint32_t kRecordsPerPage = (kPageSize - kPageHeaderSize) / kRecordSize;
constexpr uint32_t kCapacity = kPageCount * kRecordsPerPage;
constexpr uint64_t kSequenceBlockSize = 256;
constexpr uint32_t kSequenceSlotsPerPage = 8;
constexpr uint32_t kStateSlotsPerPage = 4;
static_assert(kBaseAddress + kRegionSize == 0xF4000, "bootloader boundary");
static_assert(kPageHeaderSize + kRecordsPerPage * kRecordSize == kPageSize, "page packing");
}  // namespace orun_tlp::storage_config
