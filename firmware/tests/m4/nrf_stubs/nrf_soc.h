#pragma once
#include <stdint.h>
constexpr uint32_t NRF_SUCCESS = 0;
constexpr uint32_t NRF_ERROR_INTERNAL = 3;
constexpr uint32_t NRF_ERROR_NOT_FOUND = 5;
constexpr uint32_t NRF_ERROR_BUSY = 17;
// Matches the installed S140 6.1.1 NRF_SOC_EVTS enum ordering exactly
// (nrf_soc.h): HFCLKSTARTED=0, POWER_FAILURE_WARNING=1, then these two.
constexpr uint32_t NRF_EVT_FLASH_OPERATION_SUCCESS = 2;
constexpr uint32_t NRF_EVT_FLASH_OPERATION_ERROR = 3;
uint32_t sd_flash_write(uint32_t* destination, const uint32_t* source, uint32_t words);
uint32_t sd_flash_page_erase(uint32_t page);
uint32_t sd_evt_get(uint32_t* event_id);
