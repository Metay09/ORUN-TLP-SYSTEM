// Tek ayar tablosu: settings_table.h.
#include "settings_table.h"

#include <stdio.h>
#include <string.h>

namespace orun_tlp {

// Value on the wire: settings_table.h.
const SettingRow kSettingRows[kSettingCount] = {
    {ConfigMutationKind::kSetTrackingInterval, "INTERVAL",
     kSettingDoorUsb | kSettingDoorBle, kSettingForTracker, false},
    {ConfigMutationKind::kSetServiceIntent, "SERVICES",
     kSettingDoorUsb | kSettingDoorBle, kSettingForTracker | kSettingForGateway, false},
    // The Gateway's Bluetooth is always on; the window is a Tracker setting.
    {ConfigMutationKind::kSetBleWindow, "BLE_WINDOW",
     kSettingDoorUsb | kSettingDoorBle, kSettingForTracker, false},
};

const SettingRow* findSetting(uint8_t id) {
  for (const SettingRow& row : kSettingRows)
    if (static_cast<uint8_t>(row.kind) == id) return &row;
  return nullptr;
}

const SettingRow* findSetting(const char* name, size_t length) {
  if (name == nullptr) return nullptr;
  for (const SettingRow& row : kSettingRows)
    if (strlen(row.name) == length && memcmp(row.name, name, length) == 0) return &row;
  return nullptr;
}

bool settingAllowed(const SettingRow& row, SettingDoor door, StorageRole role) {
  if ((row.doors & door) == 0) return false;
  if (door == kSettingDoorLora && row.radio) return false;
  switch (role) {
    case StorageRole::kTracker: return (row.roles & kSettingForTracker) != 0;
    case StorageRole::kGateway: return (row.roles & kSettingForGateway) != 0;
    case StorageRole::kUnassigned:
      // A blank device is being set up: nothing written to protect yet.
      return true;
    case StorageRole::kConflict: return false;
  }
  return false;
}

bool settingRequest(const SettingRow& row, ApplicationRequester requester,
                    uint32_t request_id, ApplicationAccessContext access,
                    uint32_t value, ConfigMutationRequest* out) {
  if (out == nullptr) return false;
  switch (row.kind) {
    case ConfigMutationKind::kSetTrackingInterval:
      *out = ConfigMutationRequest(requester, request_id, access, row.kind, value);
      return true;
    case ConfigMutationKind::kSetBleWindow:
      *out = ConfigMutationRequest(requester, request_id, access, row.kind, 0, 0, 0, value);
      return true;
    case ConfigMutationKind::kSetServiceIntent:
      if ((value >> 16) != 0U) return false;
      *out = ConfigMutationRequest(requester, request_id, access, row.kind, 0,
                                   static_cast<uint8_t>(value),
                                   static_cast<uint8_t>(value >> 8));
      return true;
  }
  return false;
}

uint32_t settingValue(const SettingRow& row, const config_format::Config& config) {
  switch (row.kind) {
    case ConfigMutationKind::kSetTrackingInterval: return config.tracking_interval_seconds;
    case ConfigMutationKind::kSetBleWindow: return config.ble_window_seconds;
    case ConfigMutationKind::kSetServiceIntent:
      return static_cast<uint32_t>(config.service_mode) |
             (static_cast<uint32_t>(config.requested_services) << 8);
  }
  return 0;
}

size_t formatSettingLine(const SettingRow& row, uint32_t value, StorageRole role,
                         char* out, size_t capacity) {
  if (out == nullptr || capacity == 0) return 0;
  const int n = snprintf(
      out, capacity, "APP SETTING id=%u name=%s value=%lu usb=%u ble=%u lora=%u radio=%u",
      static_cast<unsigned>(row.kind), row.name, static_cast<unsigned long>(value),
      settingAllowed(row, kSettingDoorUsb, role) ? 1U : 0U,
      settingAllowed(row, kSettingDoorBle, role) ? 1U : 0U,
      settingAllowed(row, kSettingDoorLora, role) ? 1U : 0U, row.radio ? 1U : 0U);
  if (n < 0 || static_cast<size_t>(n) >= capacity) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(n);
}

}  // namespace orun_tlp
