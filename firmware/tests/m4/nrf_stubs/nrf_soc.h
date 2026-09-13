#pragma once
#include <stdint.h>
constexpr uint32_t NRF_SUCCESS = 0;
constexpr uint32_t NRF_ERROR_BUSY = 17;
uint32_t sd_flash_write(uint32_t* destination, const uint32_t* source, uint32_t words);
uint32_t sd_flash_page_erase(uint32_t page);
