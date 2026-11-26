#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

test_dir="${1:?output directory required}"
mkdir -p "$test_dir"

flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror
       -fsanitize=address,undefined
       -fno-sanitize-recover=undefined
       -Ifirmware/tests/m3/stubs -Ifirmware/include)

gnss_sources=(firmware/src/gnss_manager.cpp
              firmware/src/gnss_utc.cpp
              firmware/src/i2c_recovery.cpp
              firmware/src/sensor_power_manager.cpp)

g++ -DORUN_REPORT_CADENCE_HOST_TEST=1 -Ifirmware/tests/startup/stubs -Ifirmware/tests/r2/stubs \
  -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/startup/test_startup.cpp "${gnss_sources[@]}" \
  firmware/src/activity_capture.cpp firmware/src/activity_window.cpp \
  firmware/src/activity_quality.cpp firmware/src/activity_auto_sampler.cpp \
  firmware/src/activity_period_evidence.cpp \
  firmware/src/accelerometer_manager.cpp \
  firmware/src/radio_manager.cpp firmware/src/lora_command_frame.cpp firmware/src/custody_ack.cpp firmware/src/radio_manager_relay_config.cpp \
  firmware/src/bridge_frame.cpp \
  firmware/src/radio_listen_policy.cpp firmware/src/radio_driver_gate.cpp \
  firmware/src/rak_device_identity.cpp firmware/src/legacy_position_mapping.cpp \
  firmware/src/network_service.cpp firmware/src/node_role.cpp \
  firmware/src/runtime_config.cpp \
  firmware/src/geofence_geometry.cpp firmware/src/geofence_area_set.cpp \
  firmware/src/geofence_runtime.cpp firmware/src/geofence_operational_state.cpp \
  firmware/src/geofence_runtime_policy.cpp \
  firmware/src/geofence_confirmation_coordinator.cpp \
  firmware/src/geofence_runtime_provider.cpp firmware/src/geofence_store.cpp \
  firmware/src/geofence_format.cpp \
  firmware/src/tlp_test_packet.cpp firmware/src/tlp_position_packet.cpp \
  firmware/src/tlp_relay_forward_packet.cpp firmware/src/tlp_v2_history_secure.cpp \
  firmware/src/history_store.cpp \
  firmware/src/history_store_forward_runtime.cpp \
  firmware/src/history_receipt_admission.cpp \
  firmware/src/history_delivery_coordinator.cpp \
  firmware/tests/startup/stubs/history_secure_crypto_stub.cpp \
  firmware/tests/startup/stubs/custody_ack_release_owner_stub.cpp \
  firmware/src/journal_format.cpp firmware/src/nrf_history_flash.cpp \
  firmware/src/nrf_config_flash.cpp firmware/src/nrf_security_flash.cpp \
  firmware/src/nrf_geofence_flash.cpp \
  firmware/src/nrf_product_flash.cpp \
  firmware/src/flash_mutation_gate.cpp \
  firmware/src/config_store.cpp firmware/src/config_format.cpp \
  firmware/src/application_request.cpp \
  firmware/src/application_status_runtime.cpp \
  firmware/src/usb_application_adapter.cpp firmware/src/settings_table.cpp \
  firmware/src/config_mutation.cpp firmware/src/service_intent.cpp \
  firmware/src/ble_application_transport.cpp \
  firmware/src/ble_application_handoff.cpp \
  firmware/src/security_store.cpp firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp \
  firmware/src/security_v3_format.cpp firmware/src/security_format.cpp \
  firmware/src/position_flow.cpp firmware/src/ble_admission_policy.cpp \
  firmware/src/loop_health.cpp firmware/src/loop_health_monitor.cpp \
  firmware/src/battery_monitor.cpp firmware/src/battery_policy.cpp firmware/src/power_state.cpp \
  firmware/src/geofence_mutation.cpp firmware/src/geofence_staging.cpp \
  firmware/src/product_period_builder.cpp firmware/src/tlp_v2_product_secure.cpp \
  firmware/src/product_record_store.cpp firmware/src/product_region.cpp \
  firmware/src/observation_store.cpp firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  -o "$test_dir/startup"

for scenario in \
    orun301_fix60 orun301_absent60 \
    orun301_fix900 orun301_absent900
do
    echo "=== $scenario ==="
    "$test_dir/startup" "$scenario"
done

echo "ORUN-301 MAIN LOOP REPORT REGRESSIONS PASS"
