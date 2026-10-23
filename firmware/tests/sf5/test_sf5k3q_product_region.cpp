#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <array>
#include "product_region.h"
#include "custody_store_product_format.h"
#include "custody_store_format.h"
#include "observation_store_format.h"

using namespace orun_tlp;
namespace pr = orun_tlp::product_region;
namespace pc = orun_tlp::custody_store_product_format;

struct FakeFlash : FlashBackend {
  std::array<uint8_t,4U*pc::kPageSize> bytes{};
  bool read_fail = false;
  FakeFlash() { bytes.fill(0xFFU); }
  bool begin() override { return true; }
  bool read(uint32_t offset,void* data,size_t size) const override {
    if (read_fail || !data || offset>bytes.size() ||
        size>bytes.size()-offset) return false;
    memcpy(data,bytes.data()+offset,size);
    return true;
  }
  FlashOpResult program(uint32_t,const void*,size_t) override {
    return FlashOpResult::kFailed;
  }
  FlashOpResult erasePage(uint32_t) override {
    return FlashOpResult::kFailed;
  }
  void magic(unsigned page,uint32_t value) {
    assert(page<4U);
    uint8_t* p=bytes.data()+page*pc::kPageSize;
    p[0]=static_cast<uint8_t>(value>>24U);
    p[1]=static_cast<uint8_t>(value>>16U);
    p[2]=static_cast<uint8_t>(value>>8U);
    p[3]=static_cast<uint8_t>(value);
  }
};

int main() {
  FakeFlash flash;
  pr::Census census{};
  assert(pr::takeCensus(flash,4,census));
  assert(census.erased==4 && census.custody==0 &&
         census.product_custody==0);
  assert(pr::mountProductCustodyDecision(census)==pr::MountDecision::kMount);
  assert(pr::mountDecision(pr::Owner::kCustody,census)==pr::MountDecision::kMount);

  flash.magic(0,pc::kPageMagic);
  assert(pr::takeCensus(flash,4,census));
  assert(census.product_custody==1 && census.erased==3 &&
         census.custody==0 && census.unknown==0);
  assert(pr::mountProductCustodyDecision(census)==pr::MountDecision::kMount);
  assert(pr::mountDecision(pr::Owner::kCustody,census)==
         pr::MountDecision::kForeignStore);
  assert(pr::mountDecision(pr::Owner::kObservation,census)==
         pr::MountDecision::kForeignStore);

  flash.magic(1,custody_store_format::kPageMagic);
  assert(pr::takeCensus(flash,4,census));
  assert(census.product_custody==1 && census.custody==1);
  assert(pr::mountProductCustodyDecision(census)==
         pr::MountDecision::kForeignStore);
  assert(pr::mountDecision(pr::Owner::kCustody,census)==
         pr::MountDecision::kForeignStore);

  flash.magic(1,observation_store_format::kPageMagic);
  assert(pr::takeCensus(flash,4,census));
  assert(census.product_custody==1 && census.observation==1);
  assert(pr::mountProductCustodyDecision(census)==
         pr::MountDecision::kForeignStore);
  assert(pr::mountDecision(pr::Owner::kObservation,census)==
         pr::MountDecision::kForeignStore);

  flash.bytes.fill(0xFFU);
  flash.bytes[2U*pc::kPageSize+300U]=0U;
  assert(pr::takeCensus(flash,4,census));
  assert(census.unknown==1 && census.erased==3);
  assert(pr::mountProductCustodyDecision(census)==
         pr::MountDecision::kNeedsPreparation);
  assert(pr::mountDecision(pr::Owner::kCustody,census)==
         pr::MountDecision::kNeedsPreparation);

  // The OCQ2 format scanner rejects an unknown companion page.
  flash.magic(0,pc::kPageMagic);
  assert(pr::takeCensus(flash,4,census));
  assert(census.product_custody==1 && census.unknown==1);
  assert(pr::mountProductCustodyDecision(census)==pr::MountDecision::kMount);
  flash.read_fail=true;
  assert(!pr::takeCensus(flash,4,census));
  puts("SF5K3Q OCQ2 / OCQ1 / OBS1 custody ownership census: PASS");
}
