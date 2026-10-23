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

# Remove the per-run /tmp build directory on any exit (pass, failure,
# INT/TERM/HUP). A caller-chosen ORUN_HOST_TEST_DIR is never removed.
# Set ORUN_KEEP_TEST_DIR=1 to keep the /tmp directory for debugging.
test_dir=""
owned_tmp_dir=0
cleanup_test_dir() {
  local status=$?
  local LC_ALL=C
  if [[ "$owned_tmp_dir" == 1 && -n "$test_dir" &&
        "$test_dir" =~ ^/tmp/orun-host-tests\.[A-Za-z0-9]{6}$ &&
        -d "$test_dir" && ! -L "$test_dir" && -O "$test_dir" ]]; then
    if [[ "${ORUN_KEEP_TEST_DIR:-0}" == 1 ]]; then
      echo "run_host_tests: keeping $test_dir (ORUN_KEEP_TEST_DIR=1)" >&2
    else
      rm -rf -- "$test_dir"
    fi
  fi
  return "$status"
}
trap cleanup_test_dir EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if [[ -n "${ORUN_HOST_TEST_DIR:-}" ]]; then
  test_dir="$(safe_build_child "$ORUN_HOST_TEST_DIR")"
  rm -rf -- "$test_dir"
  mkdir -p -- "$test_dir"
else
  test_dir=$(mktemp -d /tmp/orun-host-tests.XXXXXX)
  owned_tmp_dir=1
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
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5d_full_region_source_contract.py
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5f_store_scope_contract.py

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
g++ "${portable_flags[@]}" firmware/tests/m6/test_geofence_writer.cpp \
  firmware/src/geofence_mutation.cpp firmware/src/geofence_staging.cpp \
  firmware/src/geofence_store.cpp firmware/src/geofence_format.cpp \
  firmware/src/geofence_geometry.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp -o "$test_dir/geofence_writer"
"$test_dir/geofence_writer"
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
g++ "${flags[@]}" firmware/tests/m3/test_battery_monitor.cpp \
  firmware/src/battery_monitor.cpp -o "$test_dir/battery_monitor"
"$test_dir/battery_monitor"
g++ "${portable_flags[@]}" firmware/tests/m3/test_battery_policy.cpp \
  firmware/src/battery_policy.cpp firmware/src/geofence_runtime_policy.cpp \
  -o "$test_dir/battery_policy"
"$test_dir/battery_policy"
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
# SF5G product-custody preflight only: legacy-format guard and size model.
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5g_product_custody_preflight.py
# SF5G candidate: host-only C++ record codec (not a Gateway/runtime writer).
g++ "${portable_flags[@]}" -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5g_custody_record_candidate.cpp \
  firmware/tests/sf5/sf5g_custody_record_candidate.cpp \
  -o "$test_dir/sf5g_custody_record_candidate"
"$test_dir/sf5g_custody_record_candidate"
# SF5G next slice: distinct host-only page magic/version and reclaim intent.
# No FlashBackend writer, mount, erase, or ACK path is linked.
g++ "${portable_flags[@]}" -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5g_page_header_candidate.cpp \
  firmware/tests/sf5/sf5g_page_header_candidate.cpp \
  firmware/tests/sf5/sf5g_custody_record_candidate.cpp \
  -o "$test_dir/sf5g_page_header_candidate"
"$test_dir/sf5g_page_header_candidate"
# SF5G host NOR model: no erase, hardware FlashBackend, Gateway or ACK.
g++ "${portable_flags[@]}" -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5g_nor_page_writer_candidate.cpp \
  firmware/tests/sf5/sf5g_nor_page_writer_candidate.cpp \
  firmware/tests/sf5/sf5g_page_header_candidate.cpp \
  firmware/tests/sf5/sf5g_custody_record_candidate.cpp \
  -o "$test_dir/sf5g_nor_page_writer_candidate"
"$test_dir/sf5g_nor_page_writer_candidate"
# Host-only two+/multi-page SF5G append and safe no-erase rollover.
g++ "${portable_flags[@]}" -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5g_region_writer_candidate.cpp \
  firmware/tests/sf5/sf5g_region_writer_candidate.cpp \
  firmware/tests/sf5/sf5g_nor_page_writer_candidate.cpp \
  firmware/tests/sf5/sf5g_page_header_candidate.cpp \
  firmware/tests/sf5/sf5g_custody_record_candidate.cpp \
  -o "$test_dir/sf5g_region_writer_candidate"
"$test_dir/sf5g_region_writer_candidate"
# Host-only, read-only reclaim intent journal census; NEVER erases flash.
g++ "${portable_flags[@]}" -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5g_intent_census_candidate.cpp \
  firmware/tests/sf5/sf5g_intent_census_candidate.cpp \
  firmware/tests/sf5/sf5g_nor_page_writer_candidate.cpp \
  firmware/tests/sf5/sf5g_page_header_candidate.cpp \
  firmware/tests/sf5/sf5g_custody_record_candidate.cpp \
  -o "$test_dir/sf5g_intent_census_candidate"
"$test_dir/sf5g_intent_census_candidate"
# SF5G read-only advisory coverage checker: never erase or ACK.
g++ "${portable_flags[@]}" -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5g_reclaim_evidence_candidate.cpp \
  firmware/tests/sf5/sf5g_reclaim_evidence_candidate.cpp \
  firmware/tests/sf5/sf5g_nor_page_writer_candidate.cpp \
  firmware/tests/sf5/sf5g_page_header_candidate.cpp \
  firmware/tests/sf5/sf5g_custody_record_candidate.cpp \
  -o "$test_dir/sf5g_reclaim_evidence_candidate"
"$test_dir/sf5g_reclaim_evidence_candidate"
# Host-only Edge durable-accept admission seam; mock authenticator/replay only.
g++ "${portable_flags[@]}" -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5g_edge_accept_gate_candidate.cpp \
  firmware/tests/sf5/sf5g_edge_accept_gate_candidate.cpp \
  firmware/tests/sf5/sf5g_custody_record_candidate.cpp \
  -o "$test_dir/sf5g_edge_accept_gate_candidate"
"$test_dir/sf5g_edge_accept_gate_candidate"
# SF5H host-only Edge SQLite inbox: exact ciphertext commit/reopen, NO Edge receipts.
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5h_edge_durable_inbox.py
# SF5H read-only FIFO reader: scan complete SQLite queue; NO backend ACK/reclaim.
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5h_edge_fifo_readonly.py
# SF5H dev-only 127.0.0.1 HTTP staging: no authenticated backend acceptance.
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5h_loopback_backend_ingress.py
# SF5H: REAL AES-CCM verified backend ledger; requires Python 'cryptography'.
# Existing no-third-party host tests remain runnable if dependency is absent.
# Merge requires an explicit NEW PASS line, not just HOST_EXIT=0 with SKIP.
if python3 -c 'from cryptography.hazmat.primitives.ciphers.aead import AESCCM; from cryptography.hazmat.primitives.kdf.hkdf import HKDF' >/dev/null 2>&1; then
  PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5h_backend_verified_ledger.py
else
  echo 'SF5H backend crypto ledger: SKIP (install python3-cryptography; full verification gate remains OPEN)'
fi
# SF5I actual Ed25519 Edge signature candidate: no runtime receipt authority.
if python3 -c 'from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey' >/dev/null 2>&1; then
  PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5i_edge_durable_accept_signed.py
  PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5k2b_signed_batch.py
  PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5k2c_gateway_ack_oracle.py
else
  echo 'SF5I/SF5K2B Edge signed durable-accept/batch: SKIP (cryptography/Ed25519 absent; merge gate OPEN)'
fi
g++ -DORUN_M4P5B_HOST_TEST=1 "${portable_flags[@]}" \
  firmware/tests/m4/test_m4p5b_history_receipt_admission.cpp \
  firmware/src/history_receipt_admission.cpp \
  firmware/src/history_delivery_coordinator.cpp firmware/src/history_store.cpp \
  firmware/src/security_store.cpp firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp \
  firmware/src/security_v3_format.cpp firmware/src/security_format.cpp \
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
  firmware/src/nrf_security_flash.cpp firmware/src/nrf_geofence_flash.cpp \
  firmware/src/nrf_product_flash.cpp -o "$test_dir/m7p3_flash_gate"
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
g++ "${portable_flags[@]}" firmware/tests/m7/test_config_store_v4_format.cpp \
  firmware/src/config_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp -o "$test_dir/config_store_v4_format"
"$test_dir/config_store_v4_format"
g++ "${portable_flags[@]}" firmware/tests/m7/test_config_store_v4.cpp \
  firmware/src/config_store.cpp firmware/src/config_format.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/config_store_v4"
"$test_dir/config_store_v4"
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p5_config_store.cpp \
  firmware/src/config_store.cpp firmware/src/config_format.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/m7p5_config_store"
"$test_dir/m7p5_config_store"
g++ "${portable_flags[@]}" firmware/tests/m7/test_config_mutation.cpp \
  firmware/src/config_mutation.cpp firmware/src/config_store.cpp \
  firmware/src/config_format.cpp firmware/src/journal_format.cpp \
  firmware/src/service_intent.cpp firmware/src/runtime_config.cpp \
  firmware/src/node_role.cpp \
  firmware/src/tlp_position_packet.cpp -o "$test_dir/config_mutation"
"$test_dir/config_mutation"
g++ "${portable_flags[@]}" firmware/tests/m7/test_service_intent.cpp \
  firmware/src/service_intent.cpp firmware/src/runtime_config.cpp \
  firmware/src/node_role.cpp -o "$test_dir/service_intent"
"$test_dir/service_intent"
g++ -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/m7/test_m7p5_flash_gate.cpp firmware/src/flash_mutation_gate.cpp \
  firmware/src/nrf_history_flash.cpp firmware/src/nrf_config_flash.cpp \
  firmware/src/nrf_security_flash.cpp firmware/src/nrf_geofence_flash.cpp \
  firmware/src/nrf_product_flash.cpp -o "$test_dir/m7p5_flash_gate"
"$test_dir/m7p5_flash_gate"
g++ "${flags[@]}" firmware/tests/m7/test_m7p5_gnss_interval.cpp "${gnss_sources[@]}" \
  -o "$test_dir/m7p5_gnss_interval"
"$test_dir/m7p5_gnss_interval"
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p6_security_format.cpp \
  firmware/src/security_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp -o "$test_dir/m7p6_security_format"
"$test_dir/m7p6_security_format"
g++ "${portable_flags[@]}" firmware/tests/m7/test_m7p6_security_store.cpp \
  firmware/src/security_store.cpp firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp \
  firmware/src/security_v3_format.cpp firmware/src/security_format.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/m7p6_security_store"
"$test_dir/m7p6_security_store"
g++ -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/m7/test_m7p6f_security_gate_integration.cpp \
  firmware/src/security_store.cpp firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp \
  firmware/src/security_v3_format.cpp firmware/src/security_format.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  firmware/src/flash_mutation_gate.cpp firmware/src/nrf_history_flash.cpp \
  firmware/src/nrf_config_flash.cpp firmware/src/nrf_security_flash.cpp \
  firmware/src/nrf_geofence_flash.cpp \
  firmware/src/nrf_product_flash.cpp \
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
  firmware/src/nrf_security_flash.cpp firmware/src/nrf_geofence_flash.cpp \
  firmware/src/nrf_product_flash.cpp -o "$test_dir/m7p6_flash_gate"

g++ -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/m6/test_m6d3b_flash_gate.cpp firmware/src/flash_mutation_gate.cpp \
  firmware/src/nrf_history_flash.cpp firmware/src/nrf_config_flash.cpp \
  firmware/src/nrf_security_flash.cpp firmware/src/nrf_geofence_flash.cpp \
  firmware/src/nrf_product_flash.cpp \
  -o "$test_dir/m6d3b_flash_gate"
"$test_dir/m6d3b_flash_gate"
# SF5D5 product region: backend, gate client, ObservationStore on the region,
# one-owner-per-device decision.
g++ -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/sf5/test_sf5d5_product_region.cpp \
  firmware/src/flash_mutation_gate.cpp firmware/src/nrf_history_flash.cpp \
  firmware/src/nrf_config_flash.cpp firmware/src/nrf_security_flash.cpp \
  firmware/src/nrf_geofence_flash.cpp firmware/src/nrf_product_flash.cpp \
  firmware/src/product_region.cpp firmware/src/observation_store.cpp \
  firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  -o "$test_dir/sf5d5_product_region"
"$test_dir/sf5d5_product_region"
# SF5E physical qualification probe, run on the host: one forked process per
# boot over shared fake flash, the same command sequence as the remote script.
g++ -Ifirmware/tests/sf5/sf5e_stubs -Ifirmware/tests/m4/nrf_stubs \
  -std=c++17 -O1 -g -Wall -Wextra -Werror "${sanitizer_flags[@]}" \
  "${coverage_flags[@]}" -Ifirmware/include -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/sf5/test_sf5e_qual_probe.cpp \
  firmware/tests/sf5/sf5e_observation_qual.cpp \
  firmware/src/flash_mutation_gate.cpp firmware/src/nrf_history_flash.cpp \
  firmware/src/nrf_config_flash.cpp firmware/src/nrf_security_flash.cpp \
  firmware/src/nrf_geofence_flash.cpp firmware/src/nrf_product_flash.cpp \
  firmware/src/product_region.cpp firmware/src/observation_store.cpp \
  firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  -o "$test_dir/sf5e_qual_probe"
"$test_dir/sf5e_qual_probe"
# SF5F step 1: durable PERIODIC records (ProductRecordStore), gnu++11 like
# the target, including a reset at every write through rotation/compaction.
g++ "${b3_flags[@]}" firmware/tests/sf5/test_sf5f_product_record_store.cpp \
  firmware/src/product_record_store.cpp firmware/src/product_region.cpp \
  firmware/src/observation_store.cpp firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  firmware/src/product_period_builder.cpp firmware/src/tlp_v2_product_secure.cpp \
  -o "$test_dir/sf5f_product_record_store"
"$test_dir/sf5f_product_record_store"
"$test_dir/m7p6_flash_gate"
g++ -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/m7/test_m7p7a_flash_gate.cpp firmware/src/flash_mutation_gate.cpp \
  firmware/src/nrf_history_flash.cpp firmware/src/nrf_config_flash.cpp \
  firmware/src/nrf_security_flash.cpp firmware/src/nrf_geofence_flash.cpp \
  firmware/src/nrf_product_flash.cpp -o "$test_dir/m7p7a_flash_gate"
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
  firmware/src/radio_manager.cpp firmware/src/custody_ack.cpp firmware/src/radio_manager_relay_config.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
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
  firmware/src/radio_manager.cpp firmware/src/custody_ack.cpp firmware/src/radio_manager_relay_config.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
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
  firmware/src/radio_manager.cpp firmware/src/custody_ack.cpp firmware/src/radio_manager_relay_config.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  firmware/src/bridge_frame.cpp \
  firmware/src/radio_listen_policy.cpp firmware/src/network_service.cpp \
  firmware/src/radio_driver_gate.cpp firmware/src/rak_device_identity.cpp \
  firmware/src/legacy_position_mapping.cpp \
  firmware/src/node_role.cpp firmware/src/tlp_test_packet.cpp \
  firmware/src/tlp_position_packet.cpp firmware/src/tlp_relay_forward_packet.cpp \
  firmware/src/tlp_v2_history_secure.cpp \
  -o "$test_dir/r2"
"$test_dir/r2"
g++ -Ifirmware/tests/r2/stubs "${flags[@]}" \
  firmware/tests/sf5/test_sf5j_radio_transport.cpp "$test_dir/driver_bridge.cpp" \
  firmware/src/radio_manager.cpp firmware/src/custody_ack.cpp firmware/src/radio_manager_relay_config.cpp \
  firmware/src/tlp_v2_product_secure.cpp firmware/src/bridge_frame.cpp \
  firmware/src/radio_listen_policy.cpp firmware/src/network_service.cpp \
  firmware/src/radio_driver_gate.cpp firmware/src/rak_device_identity.cpp \
  firmware/src/legacy_position_mapping.cpp \
  firmware/src/node_role.cpp firmware/src/tlp_test_packet.cpp \
  firmware/src/tlp_position_packet.cpp firmware/src/tlp_relay_forward_packet.cpp \
  firmware/src/tlp_v2_history_secure.cpp \
  -o "$test_dir/sf5j1_radio_transport"
# Reuse SAME SF5J1 radio fake and build, but run the newly added
# SF5K3K actual radio receiver assertions under explicit test define.
g++ -DORUN_SF5K3K_ACK_RX_TEST -Ifirmware/tests/r2/stubs "${flags[@]}" \
  firmware/tests/sf5/test_sf5j_radio_transport.cpp "$test_dir/driver_bridge.cpp" \
  firmware/src/radio_manager.cpp firmware/src/custody_ack.cpp \
  firmware/src/radio_manager_relay_config.cpp \
  firmware/src/tlp_v2_product_secure.cpp firmware/src/bridge_frame.cpp \
  firmware/src/radio_listen_policy.cpp firmware/src/network_service.cpp \
  firmware/src/radio_driver_gate.cpp firmware/src/rak_device_identity.cpp \
  firmware/src/legacy_position_mapping.cpp firmware/src/node_role.cpp \
  firmware/src/tlp_test_packet.cpp firmware/src/tlp_position_packet.cpp \
  firmware/src/tlp_relay_forward_packet.cpp firmware/src/tlp_v2_history_secure.cpp \
  -o "$test_dir/sf5k3k_radio_rx"
"$test_dir/sf5k3k_radio_rx"
"$test_dir/sf5j1_radio_transport"
g++ -Ifirmware/tests/startup/stubs -Ifirmware/tests/r2/stubs \
  -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/startup/test_startup.cpp "${gnss_sources[@]}" \
  firmware/src/activity_capture.cpp firmware/src/activity_window.cpp \
  firmware/src/activity_quality.cpp firmware/src/activity_auto_sampler.cpp \
  firmware/src/activity_period_evidence.cpp \
  firmware/src/accelerometer_manager.cpp \
  firmware/src/radio_manager.cpp firmware/src/custody_ack.cpp firmware/src/radio_manager_relay_config.cpp \
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
  firmware/src/usb_application_adapter.cpp \
  firmware/src/config_mutation.cpp firmware/src/service_intent.cpp \
  firmware/src/ble_application_transport.cpp \
  firmware/src/ble_application_handoff.cpp \
  firmware/src/security_store.cpp firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp \
  firmware/src/security_v3_format.cpp firmware/src/security_format.cpp \
  firmware/src/position_flow.cpp firmware/src/ble_admission_policy.cpp \
  firmware/src/loop_health.cpp firmware/src/loop_health_monitor.cpp \
  firmware/src/battery_monitor.cpp firmware/src/battery_policy.cpp \
  firmware/src/geofence_mutation.cpp firmware/src/geofence_staging.cpp \
  firmware/src/product_period_builder.cpp firmware/src/tlp_v2_product_secure.cpp \
  firmware/src/product_record_store.cpp firmware/src/product_region.cpp \
  firmware/src/observation_store.cpp firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  -o "$test_dir/startup"
for scenario in mutex gate queue lora success advfail blefail noevent geofence geofence_persisted geofence_uncertain history_erase_i2c services_no_gnss product_services_none product_services_relay product_busy_health battery_critical geofence_usb; do
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
g++ "${portable_flags[@]}" -Wconversion -Wsign-conversion -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5b_product_secure_codec.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  -o "$test_dir/sf5b_product_secure_codec"
"$test_dir/sf5b_product_secure_codec"
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5b_product_secure_vectors.py
# SF5K1: proposal-only packet-shape checks. No receipt authentication or RF enablement.
g++ "${b3_flags[@]}" -Wconversion -Wsign-conversion -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5k1_batch_ack_candidate.cpp \
  firmware/tests/sf5/sf5k1_batch_ack_candidate.cpp \
  -o "$test_dir/sf5k1_batch_ack_candidate"
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5k1_batch_mac_reference.py
# SF5K3B: approved-wire-shape 0x05 single-object test-only HKDF/HMAC oracle;
# no Gateway provisioned state, no authenticated release or runtime linkage.
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5k3b_single_ack_reference.py
# SF5K3C host-only v3 proposed authority journal; explicitly INERT.
# The live SecurityStore continues to persist v2 with no custody grants.
g++ "${b3_flags[@]}" -Wno-conversion -Wno-sign-conversion -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5k3c_security_v3_candidate.cpp \
  firmware/tests/sf5/sf5k3c_security_v3_candidate.cpp \
  firmware/src/security_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/sf5k3c_security_v3_candidate"
# SF5K3D: inert A/B snapshot/migration/compaction power-cut model.
# Production SecurityStore remains v2; this test never writes real flash.
g++ "${b3_flags[@]}" -Wno-conversion -Wno-sign-conversion -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5k3d_security_v3_snapshot_candidate.cpp \
  firmware/tests/sf5/sf5k3d_security_v3_snapshot_candidate.cpp \
  firmware/tests/sf5/sf5k3c_security_v3_candidate.cpp \
  firmware/src/security_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/sf5k3d_security_v3_snapshot_candidate"
# SF5K3E: production SecurityStore read-only v3 integrity inspection;
# v3 can never become active authority and no write/release API is added.
g++ "${b3_flags[@]}" -Wno-conversion -Wno-sign-conversion -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5k3e_real_security_v3_readonly.cpp \
  firmware/tests/sf5/sf5k3d_security_v3_snapshot_candidate.cpp \
  firmware/tests/sf5/sf5k3c_security_v3_candidate.cpp \
  firmware/src/security_store.cpp firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp \
  firmware/src/security_v3_format.cpp firmware/src/security_format.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/sf5k3e_real_security_v3_readonly"
# SF5K3F: new product-source v3 codec, cross-checked byte for byte against
# the prior independent host-only C/D golden and power-cut models.
g++ "${b3_flags[@]}" -Wno-conversion -Wno-sign-conversion -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5k3f_security_v3_product_format.cpp \
  firmware/src/security_v3_format.cpp \
  firmware/tests/sf5/sf5k3c_security_v3_candidate.cpp \
  firmware/tests/sf5/sf5k3d_security_v3_snapshot_candidate.cpp \
  firmware/src/security_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/sf5k3f_security_v3_product_format"
"$test_dir/sf5k3f_security_v3_product_format"
# SF5K3G real FlashBackend A/B recovery preflight is deliberately inert:
# it verifies v1/v2->v3 safe election, no authority, no writes, byte cuts.
g++ "${b3_flags[@]}" -Wno-conversion -Wno-sign-conversion -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5k3g_security_v3_ab_preflight.cpp \
  firmware/src/security_v3_ab_preflight.cpp \
  firmware/src/security_v3_format.cpp \
  firmware/tests/sf5/sf5k3c_security_v3_candidate.cpp \
  firmware/tests/sf5/sf5k3d_security_v3_snapshot_candidate.cpp \
  firmware/src/security_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/sf5k3g_security_v3_ab_preflight"
"$test_dir/sf5k3g_security_v3_ab_preflight"
# SF5K3H physically mutating v3 writer exercised ONLY on a host FakeFlash;
# production SecurityStore remains a v2 writer, v3 remains unsupported.
g++ "${b3_flags[@]}" -Wno-conversion -Wno-sign-conversion -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5k3h_security_v3_page_writer.cpp \
  firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp \
  firmware/src/security_v3_format.cpp \
  firmware/tests/sf5/sf5k3c_security_v3_candidate.cpp \
  firmware/tests/sf5/sf5k3d_security_v3_snapshot_candidate.cpp \
  firmware/src/security_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/sf5k3h_security_v3_page_writer"
"$test_dir/sf5k3h_security_v3_page_writer"
# SF5K3I: ACTUAL SecurityStore v3 boot and v2->v3 migration (not a
# model or codec-only test). No Gateway grant admission or ACK release.
g++ "${b3_flags[@]}" -Wno-conversion -Wno-sign-conversion -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5k3i_real_security_store_v3.cpp \
  firmware/src/security_store.cpp \
  firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp \
  firmware/src/security_v3_format.cpp \
  firmware/tests/sf5/sf5k3c_security_v3_candidate.cpp \
  firmware/tests/sf5/sf5k3d_security_v3_snapshot_candidate.cpp \
  firmware/src/security_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/sf5k3i_real_security_store_v3"
"$test_dir/sf5k3i_real_security_store_v3"
# SF5K3J: actual production SecurityStore private policy journal path.
# There is NO authenticated Gateway ACK owner or radio/BLE caller.
g++ "${b3_flags[@]}" -Wno-conversion -Wno-sign-conversion -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5k3j_real_security_v3_policy.cpp \
  firmware/src/security_store.cpp \
  firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp \
  firmware/src/security_v3_format.cpp \
  firmware/src/security_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/sf5k3j_real_security_v3_policy"
"$test_dir/sf5k3j_real_security_v3_policy"

# SF5K3K production ACK codec + actual SecurityStore durable grant precheck.
# Explicitly NO HMAC acceptance, radio runtime authority, or release.
g++ "${b3_flags[@]}" -Wno-conversion -Wno-sign-conversion -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5k3k_custody_ack_scope.cpp \
  firmware/src/security_store.cpp firmware/src/custody_ack.cpp \
  firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp \
  firmware/src/security_v3_format.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  firmware/src/security_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/sf5k3k_custody_ack_scope"
"$test_dir/sf5k3k_custody_ack_scope"

# SF5K3L production verifier source against independently implemented
# host OpenSSL SHA256/HKDF/HMAC; no ProductRecordStore release.
g++ "${b3_flags[@]}" -Wno-conversion -Wno-sign-conversion \
  -DORUN_SF5K3L_HOST_TEST -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5k3l_custody_ack_crypto.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/src/custody_ack_verifier.cpp firmware/src/custody_ack.cpp \
  firmware/src/security_store.cpp firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp firmware/src/security_v3_format.cpp \
  firmware/src/tlp_v2_product_secure.cpp firmware/src/security_format.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  -lcrypto -o "$test_dir/sf5k3l_custody_ack_crypto"
"$test_dir/sf5k3l_custody_ack_crypto"

# SF5K3M: actual SecurityStore-authenticated ACK -> ProductRecordStore
# exclusive release marker, complete flash and reboot; no radio cutover.
g++ "${b3_flags[@]}" -Wno-conversion -Wno-sign-conversion \
  -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5k3m_real_custody_release.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/src/custody_ack_release_owner.cpp \
  firmware/src/custody_ack_product_bridge.cpp \
  firmware/src/custody_ack_verifier.cpp firmware/src/custody_ack.cpp \
  firmware/src/product_record_store.cpp firmware/src/product_region.cpp \
  firmware/src/observation_store.cpp firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  firmware/src/product_period_builder.cpp \
  firmware/src/security_store.cpp firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp firmware/src/security_v3_format.cpp \
  firmware/src/tlp_v2_product_secure.cpp firmware/src/security_format.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  -lcrypto -o "$test_dir/sf5k3m_real_custody_release"
"$test_dir/sf5k3m_real_custody_release"

"$test_dir/sf5k3e_real_security_v3_readonly"
"$test_dir/sf5k3d_security_v3_snapshot_candidate"
"$test_dir/sf5k3c_security_v3_candidate"
# SF5K2 host-only evidence candidate regression.
g++ "${b3_flags[@]}" -Wconversion -Wsign-conversion -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5k2_gateway_batch_candidate.cpp \
  firmware/tests/sf5/sf5k2_gateway_batch_candidate.cpp \
  firmware/tests/sf5/sf5g_custody_record_candidate.cpp \
  -o "$test_dir/sf5k2_gateway_batch_candidate"
# SF5K3A: host-only exact-object release progress (never RF/runtime).
g++ "${b3_flags[@]}" -Wconversion -Wsign-conversion -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5k3_release_progress_candidate.cpp \
  firmware/tests/sf5/sf5k3_release_progress_candidate.cpp \
  -o "$test_dir/sf5k3_release_progress_candidate"
"$test_dir/sf5k3_release_progress_candidate"

# SF5K3A integration: same progress logic against the ACTUAL flash-backed
# ObservationStore APIs with durable release, restart and subset ACK tests.
g++ "${b3_flags[@]}" -Wconversion -Wsign-conversion -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5k3_observation_store_port.cpp \
  firmware/tests/sf5/sf5k3_observation_store_port.cpp \
  firmware/tests/sf5/sf5k3_release_progress_candidate.cpp \
  firmware/src/observation_store.cpp firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  -o "$test_dir/sf5k3_observation_store_port"
"$test_dir/sf5k3_observation_store_port"
"$test_dir/sf5k2_gateway_batch_candidate"
"$test_dir/sf5k1_batch_ack_candidate"

g++ "${portable_flags[@]}" -Wconversion -Wsign-conversion -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_sf5_period_builder.cpp \
  firmware/src/product_period_builder.cpp firmware/src/tlp_v2_product_secure.cpp \
  -o "$test_dir/sf5_period_builder"
"$test_dir/sf5_period_builder"
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
  firmware/src/usb_application_adapter.cpp firmware/src/service_intent.cpp \
  firmware/src/node_role.cpp firmware/src/geofence_staging.cpp \
  -o "$test_dir/m7p7h_usb_adapter"
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
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/tooling/test_orun_positions.py
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/codeql/test_summarize_sarif.py
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/codeql/test_verify_bundle_install.py
# SF5K3Q OCQ2/OCQ1/OBS1 product-region isolation.
bash tools/ci/run_sf5k3q_product_region_host.sh

# SF5K3R: real Gateway HMAC signer + flash store/owner host integration.
bash tools/ci/run_sf5k3r_real_gateway_ack_chain_host.sh

# SF5K3T: privileged ACK radio source contract and real flash/HMAC host checks.
bash tools/ci/run_sf5k3t_gateway_ack_radio_host.sh

# SF5K3U: actual main-loop ACK intake source guard + real HMAC/flash owner.
bash tools/ci/run_sf5k3u_tracker_ack_runtime_host.sh

# SF5K3V: actual OBS1 flash + tracker TX pilot state, fake sealed bytes/Radio.
# This test does NOT claim AES/CC310 or physical SF11 RF qualification.
bash tools/ci/run_sf5k3v_tracker_tx_host.sh

# Gateway RF mailbox -> real authorized custody flash/HMAC -> ACK TX mock;
# production Gateway has no grant-enrollment or OCQ2 activation yet.
bash tools/ci/run_sf5k3w_gateway_product_runtime_host.sh

# SF5K3X: OCQ2-only Gateway boot gate, immutable flash ownership and
# revocation-safe eligibility. No physical provisioning or Gateway enablement.
bash tools/ci/run_sf5k3x_gateway_startup_guard_host.sh

# SF5K3Y: read-only Gateway security grant snapshot with TEST FAKE issuer
# proof, real OCQ2 boot guard and separate security/product flash owners.
bash tools/ci/run_sf5k3y_gateway_grant_reader_host.sh

# SF5K3Z: actual OpenSSL Ed25519 issuer signatures on HOST, with the
# independent exact-revision rollback checkpoint interface (mock anchor).
bash tools/ci/run_sf5k3z_gateway_issuer_rollback_host.sh

# SF5K3AA: Nordic CC310 Ed25519 adapter, real OpenSSL verifier HOST mock.
# Actual pinned target SDK verified via separate standalone PlatformIO env.
bash tools/ci/run_sf5k3aa_ed25519_host.sh

# SF5K3AB: signed append-only enrollment proposal review with mocked
# independent revision; no writer, no trusted anchor, no RF activation.
bash tools/ci/run_sf5k3ab_signed_append_preflight_host.sh

# SF5K3AC: host-only signed A/B flash transaction & power-cut model
# with fake NOR and mock revision anchor; no field firmware writer.
bash tools/ci/run_sf5k3ac_signed_append_transaction_host.sh
