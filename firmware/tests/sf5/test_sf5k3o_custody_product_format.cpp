#include "custody_store_product_format.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>

using namespace orun_tlp::custody_store_product_format;
int main() {
  uint8_t page[kPageHeaderSize]{};
  assert(!encodePageHeader(0U,page));
  assert(!encodePageHeader(UINT64_MAX,page));
  assert(encodePageHeader(1U,page));
  PageInspection ph{};
  assert(inspectPageHeader(page,sizeof(page),ph));
  assert(ph.evidence == PageEvidence::kPrepared && ph.generation==1U);
  memset(page+kHeaderActiveOffset,0,4U);
  assert(inspectPageHeader(page,sizeof(page),ph));
  assert(ph.evidence == PageEvidence::kActive);
  page[28U] ^= 1U;
  assert(inspectPageHeader(page,sizeof(page),ph));
  assert(ph.evidence == PageEvidence::kCorrupt);
  page[28U] ^= 1U;
  page[36U]=0x80U;
  assert(inspectPageHeader(page,sizeof(page),ph));
  assert(ph.evidence == PageEvidence::kPartialActivation);
  page[36U]=0xFFU;
  memset(page+32U,0xFF,4U);
  assert(inspectPageHeader(page,sizeof(page),ph));
  assert(ph.evidence == PageEvidence::kStaged);
  memset(page,0xFF,sizeof(page));
  assert(inspectPageHeader(page,sizeof(page),ph));
  assert(ph.evidence == PageEvidence::kErased);

  for(size_t length: {kEventSize,kPeriodicSize}) {
    uint8_t frame[kMaxObjectSize]{};
    for(size_t i=0U;i<length;i++) frame[i]=static_cast<uint8_t>(i*7U+3U);
    uint8_t stored[kRecordSize]{};
    assert(encodeRecord(frame,length,stored));
    RecordInspection r{};
    assert(inspectRecord(stored,sizeof(stored),r));
    assert(r.evidence == RecordEvidence::kHeld && r.object_size==length &&
           r.next_handoff_slot == 0U);
    assert(exactObject(r,frame,length));
    frame[length-1U]^=1U;
    assert(!exactObject(r,frame,length));
    frame[length-1U]^=1U;
    stored[2U]^=1U;
    assert(inspectRecord(stored,sizeof(stored),r));
    assert(r.evidence == RecordEvidence::kCorrupt);
    stored[2U]^=1U;
    stored[112U] = 0xFEU;
    assert(inspectRecord(stored,sizeof(stored),r));
    assert(r.evidence == RecordEvidence::kHeld && r.handoff_uncertain &&
           r.next_handoff_slot == 1U);
    memset(stored+116U,0,4U);
    assert(inspectRecord(stored,sizeof(stored),r));
    assert(r.evidence == RecordEvidence::kHandedOff);
    assert(exactObject(r,frame,length));
    stored[112U]=0xFFU;memset(stored+116U,0xFF,4U);
    memset(stored+108U,0xFF,4U);
    assert(inspectRecord(stored,sizeof(stored),r));
    assert(r.evidence==RecordEvidence::kStaged);
    stored[108U]=0x01U;
    assert(inspectRecord(stored,sizeof(stored),r));
    assert(r.evidence==RecordEvidence::kPartialCommit);
  }
  uint8_t input[100]{};
  uint8_t output[120]{};
  memset(output,0xAA,sizeof(output));
  assert(!encodeRecord(nullptr,100U,output));
  assert(!encodeRecord(input,73U,output));
  assert(!encodeRecord(input,85U,output));
  for(uint8_t b:output) assert(b==0xAAU);
  RecordInspection r{};
  assert(!inspectRecord(nullptr,120,r));
  assert(!inspectRecord(output,119,r));
  memset(output,0xFF,sizeof(output));
  assert(inspectRecord(output,120,r) && r.evidence==RecordEvidence::kErased);
  puts("SF5K3O PRODUCT_SECURE custody record format 84/100, CRC, reset markers: PASS");
}
