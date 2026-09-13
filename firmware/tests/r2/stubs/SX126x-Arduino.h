#pragma once

#include <stdint.h>

enum RadioModems_t { MODEM_FSK = 0, MODEM_LORA };
enum RadioState_t { RF_IDLE = 0, RF_RX_RUNNING, RF_TX_RUNNING, RF_CAD };

struct RadioEvents_t {
  void (*TxDone)(void) = nullptr;
  void (*TxTimeout)(void) = nullptr;
  void (*RxDone)(uint8_t*, uint16_t, int16_t, int8_t) = nullptr;
  void (*RxTimeout)(void) = nullptr;
  void (*RxError)(void) = nullptr;
  void (*PreAmpDetect)(void) = nullptr;
  void (*FhssChangeChannel)(uint8_t) = nullptr;
  void (*CadDone)(bool) = nullptr;
};

struct Radio_s {
  void (*Init)(RadioEvents_t*);
  RadioState_t (*GetStatus)();
  void (*SetChannel)(uint32_t);
  void (*SetRxConfig)(RadioModems_t, uint32_t, uint32_t, uint8_t, uint32_t,
                      uint16_t, uint16_t, bool, uint8_t, bool, bool, uint8_t,
                      bool, bool);
  void (*SetTxConfig)(RadioModems_t, int8_t, uint32_t, uint32_t, uint32_t,
                      uint8_t, uint16_t, bool, bool, bool, uint8_t, bool,
                      uint32_t);
  void (*Send)(uint8_t*, uint8_t);
  void (*Sleep)();
  void (*Standby)();
  void (*Rx)(uint32_t);
  void (*SetCustomSyncWord)(uint16_t);
  uint16_t (*GetSyncWord)();
};

extern const Radio_s Radio;

int lora_rak4630_init();
void BoardGetUniqueId(uint8_t* id);
