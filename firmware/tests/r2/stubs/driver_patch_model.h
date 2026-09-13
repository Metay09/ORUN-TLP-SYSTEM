#pragma once
#include <atomic>
#include "radio_driver_gate.h"

// Hardware-facing boundary only. The tested bridge functions are extracted
// verbatim from the exact transformed production dependency source.
inline std::atomic<bool> IrqFired{false};
inline bool TimerTxTimeout = false;
inline bool TimerRxTimeout = false;
constexpr unsigned IRQ_RADIO_NONE = 0;
constexpr unsigned IRQ_RADIO_ALL = 0xFFFF;
void RadioStandby();
void RadioSleep();
void SX126xSetDioIrqParams(unsigned, unsigned, unsigned, unsigned);
void SX126xClearIrqStatus(unsigned);
void RadioBgIrqProcess();
