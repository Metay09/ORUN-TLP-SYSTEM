#pragma once

// Tek ayar tablosu (sahip kararı 2026-11-11, Meshtastic AdminMessage gibi):
// her ayar burada BİR satırdır. Satırın numarası ConfigMutationKind'dir ve
// Bluetooth'ta (ileride LoRa'da) işlem numarası olarak aynen kullanılır; adı
// USB'de "APP SET <AD> <değer>" içindir. Hangi kapıdan (USB / Bluetooth /
// LoRa) ve hangi role (tasma / gateway) açık olduğu satırda yazar. Yeni bir
// ayar = bu tabloya bir satır + ConfigMutationOwner'da uygulanışı; kapılar
// tablodan okur, kapı başına ayrı liste yazılmaz.
//
// Telsiz ayarları (frekans, SF, bant, güç) LoRa'dan değiştirilmez (sahip
// kararı 2026-11-11: bağlantı kopabilir, kanal habersiz değişebilir); bu
// tabloya eklendiklerinde radio = true olur ve LoRa kapısı kendiliğinden kapanır.
//
// LoRa kapısı (kSettingDoorLora) henüz bir yazıcıya bağlı değil (adım 6);
// satırlarda bu bit o adımda açılır, şimdiden açık gösterilmez.

#include <stddef.h>
#include <stdint.h>

#include "config_format.h"
#include "config_mutation.h"
#include "device_storage_role.h"

namespace orun_tlp {

enum SettingDoor : uint8_t {
  kSettingDoorUsb = 1U << 0,
  kSettingDoorBle = 1U << 1,
  kSettingDoorLora = 1U << 2,
};

enum SettingRoles : uint8_t {
  kSettingForTracker = 1U << 0,
  kSettingForGateway = 1U << 1,
};

struct SettingRow {
  ConfigMutationKind kind;  // the number on Bluetooth / LoRa
  const char* name;         // USB: APP SET <name> <value>
  uint8_t doors;            // SettingDoor bits
  uint8_t roles;            // SettingRoles bits
  bool radio;               // a radio setting: never over LoRa
};

// Value on the wire (BLE/LoRa 32-bit value, USB decimal):
//   INTERVAL    seconds
//   SERVICES    service_mode | requested_services << 8 (service_intent.h)
//   BLE_WINDOW  seconds (0 = off)
// The rows themselves are in settings_table.cpp (gnu++11: no inline variables).
constexpr size_t kSettingCount = 3;
extern const SettingRow kSettingRows[kSettingCount];

/** The row with this number (BLE/LoRa op), or nullptr. */
const SettingRow* findSetting(uint8_t id);
/** The row with this USB name (exact, upper case), or nullptr. */
const SettingRow* findSetting(const char* name, size_t length);

/** Is this row open on this door for a device in this role? (radio rows never on LoRa) */
bool settingAllowed(const SettingRow& row, SettingDoor door, StorageRole role);

/**
 * The one mutation request for a row and its wire value. Returns false when
 * the value cannot be this row's (e.g. SERVICES with bits above 16).
 */
bool settingRequest(const SettingRow& row, ApplicationRequester requester,
                    uint32_t request_id, ApplicationAccessContext access,
                    uint32_t value, ConfigMutationRequest* out);

/** This row's current value in the saved configuration (wire encoding above). */
uint32_t settingValue(const SettingRow& row, const config_format::Config& config);

/**
 * One "APP SETTINGS?" line for a row (no newline), e.g.
 *   APP SETTING id=1 name=INTERVAL value=900 usb=1 ble=1 lora=0 radio=0
 * where usb/ble/lora say whether this device, in this role, takes the row on
 * that door. Returns the length written, 0 if it did not fit.
 */
size_t formatSettingLine(const SettingRow& row, uint32_t value, StorageRole role,
                         char* out, size_t capacity);

}  // namespace orun_tlp
