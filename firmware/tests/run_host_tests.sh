#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
test_dir=$(mktemp -d /tmp/orun-host-tests.XXXXXX)
flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
       -Ifirmware/tests/m3/stubs -Ifirmware/include)
portable_flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror
                -fsanitize=address,undefined -Ifirmware/include)
b3_flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
          -fsanitize=address,undefined -Ifirmware/include)
gnss_sources=(firmware/src/gnss_manager.cpp firmware/src/gnss_utc.cpp
              firmware/src/i2c_recovery.cpp firmware/src/sensor_power_manager.cpp)

g++ "${flags[@]}" firmware/tests/compatibility/test_legacy_packets.cpp \
  firmware/src/tlp_test_packet.cpp firmware/src/tlp_position_packet.cpp \
  firmware/src/tlp_relay_forward_packet.cpp -o "$test_dir/legacy_packets"
"$test_dir/legacy_packets"

g++ "${portable_flags[@]}" firmware/tests/b2/test_b2.cpp \
  firmware/src/legacy_position_mapping.cpp firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/b2"
"$test_dir/b2"

g++ "${b3_flags[@]}" firmware/tests/b3/test_b3.cpp \
  firmware/src/node_role.cpp -o "$test_dir/b3"
"$test_dir/b3"

g++ "${b3_flags[@]}" firmware/tests/b4/test_b4.cpp \
  firmware/src/runtime_config.cpp -o "$test_dir/b4"
"$test_dir/b4"

g++ "${flags[@]}" firmware/tests/b4/test_b4_network.cpp \
  firmware/src/network_service.cpp firmware/src/node_role.cpp \
  firmware/src/tlp_position_packet.cpp firmware/src/tlp_relay_forward_packet.cpp \
  -o "$test_dir/b4_network"
"$test_dir/b4_network"

g++ "${portable_flags[@]}" firmware/tests/m6/test_m6p1_radio_policy.cpp \
  firmware/src/radio_listen_policy.cpp -o "$test_dir/m6p1_radio_policy"
"$test_dir/m6p1_radio_policy"

g++ -Ifirmware/tests/m6/stubs "${flags[@]}" \
  firmware/tests/m6/test_m6_accelerometer.cpp \
  firmware/src/accelerometer_manager.cpp firmware/src/i2c_recovery.cpp \
  -o "$test_dir/m6_accelerometer"
"$test_dir/m6_accelerometer"

g++ -Ifirmware/tests/m6/stubs "${flags[@]}" \
  firmware/tests/m6/test_m6_activity_capture.cpp \
  firmware/src/accelerometer_manager.cpp firmware/src/i2c_recovery.cpp \
  firmware/src/activity_capture.cpp firmware/src/activity_window.cpp \
  firmware/src/activity_quality.cpp -o "$test_dir/m6_activity_capture"
"$test_dir/m6_activity_capture"

g++ "${b3_flags[@]}" firmware/tests/m6/test_m6_activity.cpp \
  firmware/src/activity_window.cpp -o "$test_dir/m6_activity"
"$test_dir/m6_activity"

g++ "${b3_flags[@]}" firmware/tests/m6/test_m6_activity_quality.cpp \
  firmware/src/activity_quality.cpp -o "$test_dir/m6_activity_quality"
"$test_dir/m6_activity_quality"

g++ "${b3_flags[@]}" firmware/tests/m6/test_m6_geofence_geometry.cpp \
  firmware/src/geofence_geometry.cpp -o "$test_dir/m6_geofence_geometry"
"$test_dir/m6_geofence_geometry"

g++ "${b3_flags[@]}" firmware/tests/m6/test_m6_geofence_domain.cpp \
  firmware/src/geofence_geometry.cpp -o "$test_dir/m6_geofence_domain"
"$test_dir/m6_geofence_domain"

g++ "${b3_flags[@]}" firmware/tests/m6/test_m6_geofence_area_set.cpp \
  firmware/src/geofence_area_set.cpp firmware/src/geofence_geometry.cpp \
  -o "$test_dir/m6_geofence_area_set"
"$test_dir/m6_geofence_area_set"

g++ "${b3_flags[@]}" firmware/tests/m6/test_m6_geofence_runtime.cpp \
  firmware/src/geofence_runtime.cpp firmware/src/geofence_area_set.cpp \
  firmware/src/geofence_geometry.cpp -o "$test_dir/m6_geofence_runtime"
"$test_dir/m6_geofence_runtime"

g++ "${flags[@]}" firmware/tests/m3/test_m3.cpp "${gnss_sources[@]}" \
  -o "$test_dir/m3"
"$test_dir/m3"
g++ "${flags[@]}" firmware/tests/r3/test_delayed_pair.cpp "${gnss_sources[@]}" \
  -o "$test_dir/r3_delayed"
"$test_dir/r3_delayed"
g++ "${flags[@]}" firmware/tests/r3/test_r3.cpp "${gnss_sources[@]}" \
  firmware/src/tlp_position_packet.cpp firmware/src/node_role.cpp -o "$test_dir/r3"
"$test_dir/r3"
g++ "${flags[@]}" firmware/tests/m4/test_m4.cpp firmware/src/history_store.cpp \
  firmware/src/journal_format.cpp firmware/src/position_flow.cpp \
  firmware/src/legacy_position_mapping.cpp firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/m4"
"$test_dir/m4"
g++ -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/m4/test_nrf_backend.cpp firmware/src/nrf_history_flash.cpp \
  firmware/src/history_store.cpp firmware/src/journal_format.cpp \
  firmware/src/position_flow.cpp firmware/src/legacy_position_mapping.cpp \
  firmware/src/tlp_position_packet.cpp -o "$test_dir/nrf_backend"
"$test_dir/nrf_backend"
g++ -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/m7/test_m7p3_flash_gate.cpp firmware/src/flash_mutation_gate.cpp \
  firmware/src/nrf_history_flash.cpp firmware/src/nrf_config_flash.cpp \
  firmware/src/nrf_security_flash.cpp -o "$test_dir/m7p3_flash_gate"
"$test_dir/m7p3_flash_gate"
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p3_history_async.cpp \
  firmware/src/history_store.cpp firmware/src/journal_format.cpp \
  firmware/src/position_flow.cpp firmware/src/legacy_position_mapping.cpp \
  firmware/src/tlp_position_packet.cpp -o "$test_dir/m7p3_history_async"
"$test_dir/m7p3_history_async"
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p5_config_format.cpp \
  firmware/src/config_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp -o "$test_dir/m7p5_config_format"
"$test_dir/m7p5_config_format"
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p5_config_store.cpp \
  firmware/src/config_store.cpp firmware/src/config_format.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/m7p5_config_store"
"$test_dir/m7p5_config_store"
g++ -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/m7/test_m7p5_flash_gate.cpp firmware/src/flash_mutation_gate.cpp \
  firmware/src/nrf_history_flash.cpp firmware/src/nrf_config_flash.cpp \
  firmware/src/nrf_security_flash.cpp -o "$test_dir/m7p5_flash_gate"
"$test_dir/m7p5_flash_gate"
g++ "${flags[@]}" firmware/tests/m7/test_m7p5_gnss_interval.cpp "${gnss_sources[@]}" \
  -o "$test_dir/m7p5_gnss_interval"
"$test_dir/m7p5_gnss_interval"
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p6_security_format.cpp \
  firmware/src/security_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp -o "$test_dir/m7p6_security_format"
"$test_dir/m7p6_security_format"
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p6_security_store.cpp \
  firmware/src/security_store.cpp firmware/src/security_format.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/m7p6_security_store"
"$test_dir/m7p6_security_store"
g++ -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/m7/test_m7p6_flash_gate.cpp firmware/src/flash_mutation_gate.cpp \
  firmware/src/nrf_history_flash.cpp firmware/src/nrf_config_flash.cpp \
  firmware/src/nrf_security_flash.cpp -o "$test_dir/m7p6_flash_gate"
"$test_dir/m7p6_flash_gate"
g++ -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/m7/test_m7p7a_flash_gate.cpp firmware/src/flash_mutation_gate.cpp \
  firmware/src/nrf_history_flash.cpp firmware/src/nrf_config_flash.cpp \
  firmware/src/nrf_security_flash.cpp -o "$test_dir/m7p7a_flash_gate"
"$test_dir/m7p7a_flash_gate"
g++ "${flags[@]}" firmware/tests/m5/test_m5.cpp \
  firmware/src/network_service.cpp firmware/src/node_role.cpp \
  firmware/src/tlp_position_packet.cpp firmware/src/tlp_relay_forward_packet.cpp \
  -o "$test_dir/m5"
"$test_dir/m5"
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/r2/test_patch_radio.py "$test_dir/driver_bridge.cpp"

g++ -Ifirmware/tests/r2/stubs "${flags[@]}" firmware/tests/b4/test_b4_radio.cpp \
  "$test_dir/driver_bridge.cpp" \
  firmware/src/radio_manager.cpp firmware/src/radio_manager_relay_config.cpp \
  firmware/src/radio_listen_policy.cpp \
  firmware/src/network_service.cpp firmware/src/radio_driver_gate.cpp \
  firmware/src/rak_device_identity.cpp firmware/src/legacy_position_mapping.cpp \
  firmware/src/node_role.cpp firmware/src/tlp_test_packet.cpp \
  firmware/src/tlp_position_packet.cpp firmware/src/tlp_relay_forward_packet.cpp \
  -o "$test_dir/b4_radio"
"$test_dir/b4_radio"

g++ -Ifirmware/tests/r2/stubs "${flags[@]}" \
  firmware/tests/m6/test_m6p1_radio.cpp "$test_dir/driver_bridge.cpp" \
  firmware/src/radio_manager.cpp firmware/src/radio_manager_relay_config.cpp \
  firmware/src/radio_listen_policy.cpp \
  firmware/src/network_service.cpp firmware/src/radio_driver_gate.cpp \
  firmware/src/rak_device_identity.cpp firmware/src/legacy_position_mapping.cpp \
  firmware/src/node_role.cpp firmware/src/tlp_test_packet.cpp \
  firmware/src/tlp_position_packet.cpp firmware/src/tlp_relay_forward_packet.cpp \
  -o "$test_dir/m6p1_radio"
"$test_dir/m6p1_radio"

g++ -Ifirmware/tests/r2/stubs "${flags[@]}" firmware/tests/r2/test_r2.cpp \
  "$test_dir/driver_bridge.cpp" \
  firmware/src/radio_manager.cpp firmware/src/radio_manager_relay_config.cpp \
  firmware/src/radio_listen_policy.cpp firmware/src/network_service.cpp \
  firmware/src/radio_driver_gate.cpp firmware/src/rak_device_identity.cpp \
  firmware/src/legacy_position_mapping.cpp \
  firmware/src/node_role.cpp firmware/src/tlp_test_packet.cpp \
  firmware/src/tlp_position_packet.cpp firmware/src/tlp_relay_forward_packet.cpp \
  -o "$test_dir/r2"
"$test_dir/r2"
g++ -Ifirmware/tests/startup/stubs -Ifirmware/tests/r2/stubs \
  -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/startup/test_startup.cpp "${gnss_sources[@]}" \
  firmware/src/activity_capture.cpp firmware/src/activity_window.cpp \
  firmware/src/activity_quality.cpp \
  firmware/src/accelerometer_manager.cpp \
  firmware/src/radio_manager.cpp firmware/src/radio_manager_relay_config.cpp \
  firmware/src/radio_listen_policy.cpp firmware/src/radio_driver_gate.cpp \
  firmware/src/rak_device_identity.cpp firmware/src/legacy_position_mapping.cpp \
  firmware/src/network_service.cpp firmware/src/node_role.cpp \
  firmware/src/runtime_config.cpp \
  firmware/src/tlp_test_packet.cpp firmware/src/tlp_position_packet.cpp \
  firmware/src/tlp_relay_forward_packet.cpp firmware/src/history_store.cpp \
  firmware/src/journal_format.cpp firmware/src/nrf_history_flash.cpp \
  firmware/src/nrf_config_flash.cpp firmware/src/nrf_security_flash.cpp \
  firmware/src/flash_mutation_gate.cpp \
  firmware/src/config_store.cpp firmware/src/config_format.cpp \
  firmware/src/security_store.cpp firmware/src/security_format.cpp \
  firmware/src/position_flow.cpp firmware/src/ble_admission_policy.cpp \
  -o "$test_dir/startup"
for scenario in mutex gate queue lora success; do
  "$test_dir/startup" "$scenario"
done
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/r4/test_patch_wire.py
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/m7/test_m7p4_patch_internalfs.py
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/m7/test_m7p7a_patch_ble_flash.py
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p7b_ble_admission_policy.cpp \
  firmware/src/ble_admission_policy.cpp -o "$test_dir/m7p7b_ble_admission_policy"
"$test_dir/m7p7b_ble_admission_policy"
g++ "${flags[@]}" firmware/tests/r4/test_r4.cpp "${gnss_sources[@]}" \
  firmware/src/watchdog_manager.cpp -o "$test_dir/r4"
"$test_dir/r4"
g++ -DNRF52_SERIES -Ifirmware/tests/r4/stubs -Ifirmware/include \
  -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  firmware/tests/r4/test_watchdog_nrf.cpp firmware/src/watchdog_manager.cpp \
  -o "$test_dir/r4_watchdog_nrf"
"$test_dir/r4_watchdog_nrf"
