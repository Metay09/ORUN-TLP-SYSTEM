#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
repo_root="$(pwd -P)"
build_root="$(realpath -m -- "$repo_root/build")"

safe_build_child() {
  local requested="$1"
  local resolved
  resolved="$(realpath -m -- "$requested")"
  if [[ "$resolved" != "$build_root/"* ]]; then
    echo "ERROR: tooling output path must be a child of $build_root: $requested" >&2
    exit 2
  fi
  printf '%s\n' "$resolved"
}

if [[ -n "${ORUN_HOST_TEST_DIR:-}" ]]; then
  test_dir="$(safe_build_child "$ORUN_HOST_TEST_DIR")"
  rm -rf -- "$test_dir"
  mkdir -p -- "$test_dir"
else
  test_dir=$(mktemp -d /tmp/orun-host-tests.XXXXXX)
fi

coverage_flags=()
if [[ "${ORUN_HOST_COVERAGE:-0}" == "1" ]]; then
  coverage_flags=(--coverage -fprofile-abs-path)
fi

sanitizer_flags=()
case "${ORUN_HOST_SANITIZERS:-1}" in
  1)
    sanitizer_flags=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
    ;;
  0)
    echo "ORUN host sanitizers: DISABLED by explicit ORUN_HOST_SANITIZERS=0" >&2
    ;;
  *)
    echo "ERROR: ORUN_HOST_SANITIZERS must be exactly 0 or 1." >&2
    exit 2
    ;;
esac

flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror "${sanitizer_flags[@]}"
       "${coverage_flags[@]}" -Ifirmware/tests/m3/stubs -Ifirmware/include)
portable_flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror
                "${sanitizer_flags[@]}" "${coverage_flags[@]}" -Ifirmware/include)
b3_flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
          "${sanitizer_flags[@]}" "${coverage_flags[@]}" -Ifirmware/include)
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
  firmware/src/activity_quality.cpp firmware/src/activity_auto_sampler.cpp \
  firmware/src/activity_period_evidence.cpp \
  -o "$test_dir/m6_activity_capture"
"$test_dir/m6_activity_capture"

g++ "${b3_flags[@]}" firmware/tests/m6/test_m6_activity.cpp \
  firmware/src/activity_window.cpp -o "$test_dir/m6_activity"
"$test_dir/m6_activity"

g++ "${b3_flags[@]}" firmware/tests/m6/test_m6_activity_quality.cpp \
  firmware/src/activity_quality.cpp -o "$test_dir/m6_activity_quality"
"$test_dir/m6_activity_quality"

PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/test_storage_layout_policy.py
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5d_flash_layout_preflight.py
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5d_dfu_probe_source_contract.py

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

g++ "${b3_flags[@]}" firmware/tests/m6/test_m6d3a_geofence_format.cpp \
  firmware/src/geofence_format.cpp firmware/src/geofence_geometry.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/m6d3a_geofence_format"

g++ "${portable_flags[@]}" firmware/tests/m6/test_m6d3b_geofence_store.cpp \
  firmware/src/geofence_store.cpp firmware/src/geofence_format.cpp \
  firmware/src/geofence_geometry.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/m6d3b_geofence_store"
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/m6/test_m6d3b_source_contract.py

g++ "${b3_flags[@]}" firmware/tests/m6/test_m6d3c_geofence_runtime_provider.cpp \
  firmware/src/geofence_runtime_provider.cpp \
  firmware/src/geofence_confirmation_coordinator.cpp \
  firmware/src/geofence_runtime_policy.cpp firmware/src/geofence_runtime.cpp \
  firmware/src/geofence_operational_state.cpp firmware/src/geofence_area_set.cpp \
  firmware/src/geofence_format.cpp firmware/src/geofence_geometry.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/m6d3c_geofence_runtime_provider"
"$test_dir/m6d3c_geofence_runtime_provider"
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/m6/test_m6d3c_source_contract.py

"$test_dir/m6d3b_geofence_store"
"$test_dir/m6d3a_geofence_format"

g++ "${b3_flags[@]}" \
  firmware/tests/m6/test_m6d1_geofence_operational_state.cpp \
  firmware/src/geofence_operational_state.cpp \
  -o "$test_dir/m6d1_geofence_operational_state"
"$test_dir/m6d1_geofence_operational_state"

g++ "${b3_flags[@]}" \
  firmware/tests/m6/test_m6d2_geofence_runtime_policy.cpp \
  firmware/src/geofence_runtime_policy.cpp \
  -o "$test_dir/m6d2_geofence_runtime_policy"
"$test_dir/m6d2_geofence_runtime_policy"

g++ "${b3_flags[@]}" \
  firmware/tests/m6/test_m6d2_geofence_confirmation_coordinator.cpp \
  firmware/src/geofence_confirmation_coordinator.cpp \
  firmware/src/geofence_runtime_policy.cpp firmware/src/geofence_runtime.cpp \
  firmware/src/geofence_operational_state.cpp firmware/src/geofence_area_set.cpp \
  firmware/src/geofence_geometry.cpp \
  -o "$test_dir/m6d2_geofence_confirmation_coordinator"
"$test_dir/m6d2_geofence_confirmation_coordinator"

g++ "${flags[@]}" firmware/tests/m6/test_m6d2_gnss_continuation.cpp \
  "${gnss_sources[@]}" -o "$test_dir/m6d2_gnss_continuation"
"$test_dir/m6d2_gnss_continuation"

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
g++ "${portable_flags[@]}" \
  firmware/tests/m4/test_m4p5a_history_delivery_coordinator.cpp \
  firmware/src/history_delivery_coordinator.cpp firmware/src/history_store.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/m4p5a_history_delivery_coordinator"
"$test_dir/m4p5a_history_delivery_coordinator"
# SF4B is compiled as GNU++11 here to match the RAK4630 Arduino target.
# This deliberately catches target-language compatibility regressions that a
# C++17-only host build can hide.
g++ "${b3_flags[@]}" firmware/tests/sf4/test_sf4b_custody_store.cpp \
  firmware/src/custody_store.cpp firmware/src/custody_store_format.cpp \
  -o "$test_dir/sf4b_custody_store"
"$test_dir/sf4b_custody_store"
# SF5C portable storage format has no physical nRF address ownership yet.
g++ "${b3_flags[@]}" firmware/tests/sf5/test_sf5c_observation_store_format.cpp \
  firmware/src/observation_store_format.cpp \
  -o "$test_dir/sf5c_observation_store_format"
"$test_dir/sf5c_observation_store_format"
# SF5C bounded control payload codecs.
g++ "${b3_flags[@]}" firmware/tests/sf5/test_sf5c_observation_store_control.cpp \
  firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  -o "$test_dir/sf5c_observation_store_control"
"$test_dir/sf5c_observation_store_control"
# SF5C portable ObservationStore core: no physical nRF address ownership.
g++ "${b3_flags[@]}" firmware/tests/sf5/test_sf5c_observation_store_core.cpp \
  firmware/src/observation_store.cpp firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  -o "$test_dir/sf5c_observation_store_core"
"$test_dir/sf5c_observation_store_core"
# SF5C bounded control journal semantics and compaction.
g++ "${b3_flags[@]}" firmware/tests/sf5/test_sf5c_observation_store_controls.cpp \
  firmware/src/observation_store.cpp firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  -o "$test_dir/sf5c_observation_store_controls"
"$test_dir/sf5c_observation_store_controls"
# SF5C oldest-first rotation, persistent gap accounting and power-cut recovery.
g++ "${b3_flags[@]}" firmware/tests/sf5/test_sf5c_observation_store_rotation.cpp \
  firmware/src/observation_store.cpp firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  -o "$test_dir/sf5c_observation_store_rotation"
"$test_dir/sf5c_observation_store_rotation"
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf4/test_sf4b_capacity_model.py
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf4/test_sf4b_source_contract.py
g++ -DORUN_M4P5B_HOST_TEST=1 "${portable_flags[@]}" \
  firmware/tests/m4/test_m4p5b_history_receipt_admission.cpp \
  firmware/src/history_receipt_admission.cpp \
  firmware/src/history_delivery_coordinator.cpp firmware/src/history_store.cpp \
  firmware/src/security_store.cpp firmware/src/security_format.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  firmware/src/tlp_v2_history_secure.cpp \
  -o "$test_dir/m4p5b_history_receipt_admission"
"$test_dir/m4p5b_history_receipt_admission"
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
  firmware/src/nrf_security_flash.cpp firmware/src/nrf_geofence_flash.cpp -o "$test_dir/m7p3_flash_gate"
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
g++ "${portable_flags[@]}" firmware/tests/m7/test_config_store_v2_format.cpp \
  firmware/src/config_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp -o "$test_dir/config_store_v2_format"
"$test_dir/config_store_v2_format"
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p5_config_store.cpp \
  firmware/src/config_store.cpp firmware/src/config_format.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/m7p5_config_store"
"$test_dir/m7p5_config_store"
g++ "${portable_flags[@]}" firmware/tests/m7/test_config_mutation.cpp \
  firmware/src/config_mutation.cpp firmware/src/config_store.cpp \
  firmware/src/config_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp -o "$test_dir/config_mutation"
"$test_dir/config_mutation"
g++ -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/m7/test_m7p5_flash_gate.cpp firmware/src/flash_mutation_gate.cpp \
  firmware/src/nrf_history_flash.cpp firmware/src/nrf_config_flash.cpp \
  firmware/src/nrf_security_flash.cpp firmware/src/nrf_geofence_flash.cpp -o "$test_dir/m7p5_flash_gate"
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
  firmware/tests/m7/test_m7p6f_security_gate_integration.cpp \
  firmware/src/security_store.cpp firmware/src/security_format.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  firmware/src/flash_mutation_gate.cpp firmware/src/nrf_history_flash.cpp \
  firmware/src/nrf_config_flash.cpp firmware/src/nrf_security_flash.cpp \
  firmware/src/nrf_geofence_flash.cpp \
  -o "$test_dir/m7p6f_security_gate_integration"
"$test_dir/m7p6f_security_gate_integration"
g++ -Ifirmware/tests/m7 "${portable_flags[@]}" \
  firmware/tests/m7/test_m7p6e_crypto_contract.cpp \
  -o "$test_dir/m7p6e_crypto_contract"
"$test_dir/m7p6e_crypto_contract"
g++ -Ifirmware/tests/m7 "${portable_flags[@]}" \
  firmware/tests/m7/test_m7p6e_pairing_evidence.cpp \
  -o "$test_dir/m7p6e_pairing_evidence"
"$test_dir/m7p6e_pairing_evidence"
g++ -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/m7/test_m7p6_flash_gate.cpp firmware/src/flash_mutation_gate.cpp \
  firmware/src/nrf_history_flash.cpp firmware/src/nrf_config_flash.cpp \
  firmware/src/nrf_security_flash.cpp firmware/src/nrf_geofence_flash.cpp -o "$test_dir/m7p6_flash_gate"

g++ -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/m6/test_m6d3b_flash_gate.cpp firmware/src/flash_mutation_gate.cpp \
  firmware/src/nrf_history_flash.cpp firmware/src/nrf_config_flash.cpp \
  firmware/src/nrf_security_flash.cpp firmware/src/nrf_geofence_flash.cpp \
  -o "$test_dir/m6d3b_flash_gate"
"$test_dir/m6d3b_flash_gate"
"$test_dir/m7p6_flash_gate"
g++ -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/m7/test_m7p7a_flash_gate.cpp firmware/src/flash_mutation_gate.cpp \
  firmware/src/nrf_history_flash.cpp firmware/src/nrf_config_flash.cpp \
  firmware/src/nrf_security_flash.cpp firmware/src/nrf_geofence_flash.cpp -o "$test_dir/m7p7a_flash_gate"
"$test_dir/m7p7a_flash_gate"
g++ "${flags[@]}" firmware/tests/m5/test_m5.cpp \
  firmware/src/network_service.cpp firmware/src/node_role.cpp \
  firmware/src/tlp_position_packet.cpp firmware/src/tlp_relay_forward_packet.cpp \
  -o "$test_dir/m5"
"$test_dir/m5"
g++ "${flags[@]}" firmware/tests/m5/test_bridge_frame.cpp \
  firmware/src/bridge_frame.cpp firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/bridge_frame"
"$test_dir/bridge_frame"
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/r2/test_patch_radio.py "$test_dir/driver_bridge.cpp"

g++ -Ifirmware/tests/r2/stubs "${flags[@]}" firmware/tests/b4/test_b4_radio.cpp \
  "$test_dir/driver_bridge.cpp" \
  firmware/src/radio_manager.cpp firmware/src/radio_manager_relay_config.cpp \
  firmware/src/bridge_frame.cpp \
  firmware/src/radio_listen_policy.cpp \
  firmware/src/network_service.cpp firmware/src/radio_driver_gate.cpp \
  firmware/src/rak_device_identity.cpp firmware/src/legacy_position_mapping.cpp \
  firmware/src/node_role.cpp firmware/src/tlp_test_packet.cpp \
  firmware/src/tlp_position_packet.cpp firmware/src/tlp_relay_forward_packet.cpp \
  firmware/src/tlp_v2_history_secure.cpp \
  -o "$test_dir/b4_radio"
"$test_dir/b4_radio"

g++ -Ifirmware/tests/r2/stubs "${flags[@]}" \
  firmware/tests/m6/test_m6p1_radio.cpp "$test_dir/driver_bridge.cpp" \
  firmware/src/radio_manager.cpp firmware/src/radio_manager_relay_config.cpp \
  firmware/src/bridge_frame.cpp \
  firmware/src/radio_listen_policy.cpp \
  firmware/src/network_service.cpp firmware/src/radio_driver_gate.cpp \
  firmware/src/rak_device_identity.cpp firmware/src/legacy_position_mapping.cpp \
  firmware/src/node_role.cpp firmware/src/tlp_test_packet.cpp \
  firmware/src/tlp_position_packet.cpp firmware/src/tlp_relay_forward_packet.cpp \
  firmware/src/tlp_v2_history_secure.cpp \
  -o "$test_dir/m6p1_radio"
"$test_dir/m6p1_radio"

g++ -Ifirmware/tests/r2/stubs "${flags[@]}" firmware/tests/r2/test_r2.cpp \
  "$test_dir/driver_bridge.cpp" \
  firmware/src/radio_manager.cpp firmware/src/radio_manager_relay_config.cpp \
  firmware/src/bridge_frame.cpp \
  firmware/src/radio_listen_policy.cpp firmware/src/network_service.cpp \
  firmware/src/radio_driver_gate.cpp firmware/src/rak_device_identity.cpp \
  firmware/src/legacy_position_mapping.cpp \
  firmware/src/node_role.cpp firmware/src/tlp_test_packet.cpp \
  firmware/src/tlp_position_packet.cpp firmware/src/tlp_relay_forward_packet.cpp \
  firmware/src/tlp_v2_history_secure.cpp \
  -o "$test_dir/r2"
"$test_dir/r2"
g++ -Ifirmware/tests/startup/stubs -Ifirmware/tests/r2/stubs \
  -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/startup/test_startup.cpp "${gnss_sources[@]}" \
  firmware/src/activity_capture.cpp firmware/src/activity_window.cpp \
  firmware/src/activity_quality.cpp firmware/src/activity_auto_sampler.cpp \
  firmware/src/activity_period_evidence.cpp \
  firmware/src/accelerometer_manager.cpp \
  firmware/src/radio_manager.cpp firmware/src/radio_manager_relay_config.cpp \
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
  firmware/src/journal_format.cpp firmware/src/nrf_history_flash.cpp \
  firmware/src/nrf_config_flash.cpp firmware/src/nrf_security_flash.cpp \
  firmware/src/nrf_geofence_flash.cpp \
  firmware/src/flash_mutation_gate.cpp \
  firmware/src/config_store.cpp firmware/src/config_format.cpp \
  firmware/src/application_request.cpp \
  firmware/src/application_status_runtime.cpp \
  firmware/src/usb_application_adapter.cpp \
  firmware/src/config_mutation.cpp \
  firmware/src/ble_application_transport.cpp \
  firmware/src/ble_application_handoff.cpp \
  firmware/src/security_store.cpp firmware/src/security_format.cpp \
  firmware/src/position_flow.cpp firmware/src/ble_admission_policy.cpp \
  firmware/src/loop_health.cpp firmware/src/loop_health_monitor.cpp \
  -o "$test_dir/startup"
for scenario in mutex gate queue lora success advfail blefail noevent geofence geofence_persisted geofence_uncertain history_erase_i2c; do
  "$test_dir/startup" "$scenario"
done
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/r4/test_patch_wire.py
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/r4/test_patch_loop_stack.py
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/m7/test_m7p6g_delegated_kdf_vectors.py
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p6h_delegated_secure_codec.cpp \
  firmware/src/tlp_v2_delegated_secure_app.cpp \
  -o "$test_dir/m7p6h_delegated_secure_codec"
g++ "${portable_flags[@]}" firmware/tests/m4/test_m4p4_history_secure_codec.cpp \
  firmware/src/tlp_v2_history_secure.cpp \
  -o "$test_dir/m4p4_history_secure_codec"
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/m4/test_m4p4_history_secure_vectors.py
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p6i_security_traffic_bytes.cpp \
  -o "$test_dir/m7p6i_security_traffic_bytes"
"$test_dir/m7p6i_security_traffic_bytes"
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/m7/test_m7p6i_history_crypto_vectors.py
"$test_dir/m4p4_history_secure_codec"
"$test_dir/m7p6h_delegated_secure_codec"
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/m7/test_m7p4_patch_internalfs.py
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/m7/test_m7p7a_patch_ble_flash.py
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p7b_ble_admission_policy.cpp \
  firmware/src/ble_admission_policy.cpp -o "$test_dir/m7p7b_ble_admission_policy"
"$test_dir/m7p7b_ble_admission_policy"
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p7b_flash_probe.cpp \
  firmware/src/config_store.cpp firmware/src/config_format.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/m7p7b_flash_probe"
"$test_dir/m7p7b_flash_probe"
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p7d_app_request.cpp \
  firmware/src/application_request.cpp firmware/src/config_store.cpp \
  firmware/src/config_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp -o "$test_dir/m7p7d_app_request"
"$test_dir/m7p7d_app_request"
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p7h_app_status.cpp \
  firmware/src/application_request.cpp firmware/src/config_store.cpp \
  firmware/src/config_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp -o "$test_dir/m7p7h_app_status"
"$test_dir/m7p7h_app_status"
g++ "${flags[@]}" firmware/tests/m7/test_m7p7h_usb_adapter.cpp \
  firmware/src/usb_application_adapter.cpp -o "$test_dir/m7p7h_usb_adapter"
"$test_dir/m7p7h_usb_adapter"
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p7f_ble_application_transport.cpp \
  firmware/src/ble_application_transport.cpp firmware/src/application_request.cpp \
  firmware/src/config_store.cpp firmware/src/config_format.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/m7p7f_ble_application_transport"
"$test_dir/m7p7f_ble_application_transport"
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p7h_ble_status_transport.cpp \
  firmware/src/ble_application_transport.cpp firmware/src/application_request.cpp \
  firmware/src/config_store.cpp firmware/src/config_format.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/m7p7h_ble_status_transport"
"$test_dir/m7p7h_ble_status_transport"
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/m7/test_m7p7h_source_contract.py
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p7i_location_owner.cpp \
  -o "$test_dir/m7p7i_location_owner"
"$test_dir/m7p7i_location_owner"
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/m7/test_m7p7i_source_contract.py
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p7g_ble_application_handoff.cpp \
  firmware/src/ble_application_handoff.cpp -o "$test_dir/m7p7g_ble_application_handoff"
"$test_dir/m7p7g_ble_application_handoff"
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/m7/test_m7p7g_source_contract.py
g++ "${flags[@]}" firmware/tests/r4/test_r4.cpp "${gnss_sources[@]}" \
  firmware/src/watchdog_manager.cpp -o "$test_dir/r4"
"$test_dir/r4"
g++ -DNRF52_SERIES -Ifirmware/tests/r4/stubs -Ifirmware/include \
  -std=c++17 -O1 -g -Wall -Wextra -Werror "${sanitizer_flags[@]}" \
  "${coverage_flags[@]}" \
  firmware/tests/r4/test_watchdog_nrf.cpp firmware/src/watchdog_manager.cpp \
  -o "$test_dir/r4_watchdog_nrf"
"$test_dir/r4_watchdog_nrf"
g++ -DNRF52_SERIES -Ifirmware/tests/r4/stubs -Ifirmware/include \
  -std=c++17 -O1 -g -Wall -Wextra -Werror "${sanitizer_flags[@]}" \
  "${coverage_flags[@]}" \
  firmware/tests/r4/test_loop_health.cpp firmware/src/loop_health_monitor.cpp \
  firmware/src/loop_health.cpp -o "$test_dir/r4_loop_health"
"$test_dir/r4_loop_health"

PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/tooling/test_tooling_contract.py
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/codeql/test_summarize_sarif.py
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/codeql/test_verify_bundle_install.py
