#define ORUN_M4P5B_HOST_TEST 1
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <array>
#include <type_traits>

#include "history_receipt_admission.h"
#include "journal_format.h"
#include "storage_config.h"
#include "tlp_position_packet.h"

using namespace orun_tlp;
using namespace orun_tlp::journal_format;
using namespace orun_tlp::security_format;
using namespace orun_tlp::storage_config;

namespace orun_tlp {
struct HistoryReceiptAdmissionTestPeer {
  static AuthenticatedBackendDurableReceipt make(
      const tlp::HistorySecurePacket& packet,
      const tlp::BackendDurableReceiptPlaintext& receipt,
      const uint8_t (&credential_id)[security_format::kCredentialIdSize]) {
    AuthenticatedBackendDurableReceipt authenticated;
    authenticated.packet_ = packet;
    authenticated.receipt_ = receipt;
    memcpy(authenticated.authenticated_credential_id_,
           credential_id, sizeof(credential_id));
    return authenticated;
  }
};
}  // namespace orun_tlp

namespace {

constexpr uint64_t kDevice = UINT64_C(0x123456789ABCDEF0);
constexpr uint64_t kHistoryIncarnation = UINT64_C(0x1122334455667788);
constexpr uint8_t kCredentialSeed = 0x40;
constexpr uint64_t kSecondHistoryIncarnation =
    UINT64_C(0x8877665544332211);

static_assert(
    !std::is_aggregate<AuthenticatedBackendDurableReceipt>::value,
    "authenticated receipt must not expose aggregate packet/plaintext binding");

template <size_t Pages>
class RamFlash final : public FlashBackend {
 public:
  static constexpr size_t kSize = Pages * kPageSize;

  RamFlash() { bytes.fill(0xFF); }

  bool begin() override { return true; }

  bool read(uint32_t offset, void* data, size_t size) const override {
    if (read_fail_countdown == 0) {
      read_fail_countdown = -1;
      ++read_failures;
      return false;
    }
    if (read_fail_countdown > 0) --read_fail_countdown;
    if (data == nullptr || offset > bytes.size() ||
        size > bytes.size() - offset)
      return false;
    memcpy(data, bytes.data() + offset, size);
    return true;
  }

  void failOneReadAfter(uint32_t successful_reads) {
    read_fail_countdown = static_cast<int64_t>(successful_reads);
  }

  FlashOpResult program(uint32_t offset, const void* data,
                        size_t size) override {
    ++program_calls;
    if (data == nullptr || size == 0 ||
        (offset & 3U) != 0 || (size & 3U) != 0 ||
        offset > bytes.size() || size > bytes.size() - offset)
      return FlashOpResult::kFailed;

    const auto* source = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
      if ((bytes[offset + i] & source[i]) != source[i])
        return FlashOpResult::kFailed;
    }
    for (size_t i = 0; i < size; ++i)
      bytes[offset + i] &= source[i];
    return FlashOpResult::kDone;
  }

  FlashOpResult erasePage(uint32_t page) override {
    ++erase_calls;
    if (page >= Pages) return FlashOpResult::kFailed;
    memset(bytes.data() + size_t(page) * kPageSize, 0xFF, kPageSize);
    return FlashOpResult::kDone;
  }

  std::array<uint8_t, kSize> bytes{};
  uint32_t program_calls = 0;
  uint32_t erase_calls = 0;
  mutable int64_t read_fail_countdown = -1;
  mutable uint32_t read_failures = 0;
};

class TestHistoryIncarnationSource final : public HistoryIncarnationSource {
 public:
  bool generate(uint64_t& incarnation) override {
    incarnation = next;
    return incarnation != 0;
  }

  uint64_t next = kHistoryIncarnation;
};

void settleHistory(HistoryStore& store) {
  for (unsigned pass = 0; pass < 300 && store.busy(); ++pass)
    store.poll();
  assert(!store.busy());
}

void startHistory(HistoryStore& store,
                  uint64_t expected_incarnation = kHistoryIncarnation) {
  assert(store.begin(kDevice));
  settleHistory(store);
  if (!store.canAppend()) {
    assert(!store.prepareAppend());
    settleHistory(store);
  }
  assert(store.ready());
  assert(store.canAppend());
  assert(store.incarnation() == expected_incarnation);
}

void settleSecurity(SecurityStore& store) {
  for (unsigned pass = 0; pass < 3000 && store.busy(); ++pass)
    store.poll();
  assert(!store.busy());
}

void fillCredentialId(
    uint8_t (&id)[kCredentialIdSize],
    uint8_t seed = kCredentialSeed) {
  for (size_t i = 0; i < kCredentialIdSize; ++i)
    id[i] = static_cast<uint8_t>(seed + i);
}

void fillRoot(uint8_t (&root)[kKRootSize],
              uint8_t seed = kCredentialSeed) {
  for (size_t i = 0; i < kKRootSize; ++i)
    root[i] = static_cast<uint8_t>(seed * 3U + i);
}

void startSecurity(SecurityStore& store) {
  assert(store.begin(DeviceIdentity::fromLegacyUint64(kDevice)));
  uint8_t id[kCredentialIdSize]{};
  uint8_t root[kKRootSize]{};
  fillCredentialId(id);
  fillRoot(root);
  assert(store.commitCredential(id, 1, root));
  settleSecurity(store);
  bool success = false;
  assert(store.takeCommitResult(success));
  assert(success);
  assert(store.ready());
  assert(store.state() == SecurityState::kProvisioned);
}

HistoryStore::Record allocateRecord(HistoryStore& store, int32_t latitude) {
  uint32_t sequence = 0;
  HistoryStore::Record record{};
  if (!store.nextSequence(sequence, record.identity)) {
    assert(store.busy());
    settleHistory(store);
    assert(store.nextSequence(sequence, record.identity));
  }

  tlp::PositionPacket packet{};
  packet.source_device_id = kDevice;
  packet.sequence_number = sequence;
  packet.gnss_utc_epoch_seconds = 0x11223344U + sequence;
  packet.latitude_e7 = latitude;
  packet.longitude_e7 = -290000000;
  packet.altitude_mm = 1234;
  packet.hdop_x100 = 150;
  packet.satellites = 8;
  packet.flags = tlp::kPositionFlagValidFix |
                 tlp::kPositionFlagValidUtcTime |
                 tlp::kPositionFlag3dFix;
  assert(tlp::serializePositionPacket(
      packet, record.packet, sizeof(record.packet)));
  return record;
}

void appendRecord(HistoryStore& store, const HistoryStore::Record& record) {
  assert(store.append(record.packet, record.identity));
  settleHistory(store);
  bool success = false;
  assert(store.takeAppendResult(success));
  assert(success);
}

tlp::BackendDurableReceiptPlaintext oneReceipt(uint64_t identity) {
  tlp::BackendDurableReceiptPlaintext receipt{};
  receipt.count = 1;
  receipt.history_record_identities[0] = identity;
  return receipt;
}

tlp::BackendDurableReceiptPlaintext recordReceipt(
    const HistoryStore::Record* records, size_t first, uint8_t count) {
  assert(records != nullptr);
  assert(count > 0 && count <= tlp::kHistoryReceiptMaxIdentities);
  tlp::BackendDurableReceiptPlaintext receipt{};
  receipt.count = count;
  for (uint8_t i = 0; i < count; ++i)
    receipt.history_record_identities[i] = records[first + i].identity;
  return receipt;
}

tlp::HistorySecurePacket receiptPacket(
    const tlp::BackendDurableReceiptPlaintext& receipt,
    uint64_t security_counter,
    uint32_t key_epoch = 1,
    uint64_t incarnation = kHistoryIncarnation) {
  tlp::HistorySecurePacket packet{};
  packet.security_context = tlp::kHistorySecurityContextBackendA2d;
  packet.app_family = tlp::kHistoryAppFamilyBackendDurableReceipt;
  packet.path_flags = 0;
  packet.ciphertext_len = static_cast<uint8_t>(
      tlp::backendDurableReceiptPlaintextSize(receipt.count));
  packet.device_id = kDevice;
  packet.key_epoch = key_epoch;
  packet.security_counter = security_counter;
  packet.history_incarnation = incarnation;
  assert(tlp::validateHistorySecurePacket(packet));
  return packet;
}

AuthenticatedBackendDurableReceipt authenticatedReceipt(
    const tlp::HistorySecurePacket& packet,
    const tlp::BackendDurableReceiptPlaintext& receipt,
    const uint8_t (&credential_id)[kCredentialIdSize]) {
  return HistoryReceiptAdmissionTestPeer::make(
      packet, receipt, credential_id);
}

struct Fixture {
  RamFlash<kPageCount> history_flash;
  RamFlash<kFutureSecurityRegionPages> security_flash;
  TestHistoryIncarnationSource incarnation_source;
  HistoryStore history{history_flash, &incarnation_source};
  SecurityStore security{security_flash, security_flash};
  HistoryDeliveryCoordinator delivery{history};
  HistoryReceiptAdmissionCoordinator admission{security, delivery};

  Fixture() {
    startHistory(history);
    startSecurity(security);
  }

  void credentialId(
      uint8_t (&out)[kCredentialIdSize],
      uint8_t seed = kCredentialSeed) const {
    fillCredentialId(out, seed);
  }
};

void replayAcceptanceStrictlyPrecedesDelivery() {
  Fixture fixture;
  const auto first = allocateRecord(fixture.history, 410000001);
  appendRecord(fixture.history, first);

  const auto receipt = oneReceipt(first.identity);
  const auto packet = receiptPacket(receipt, 1);
  uint8_t credential_id[kCredentialIdSize]{};
  fixture.credentialId(credential_id);

  assert(fixture.admission.submitAuthenticatedReceipt(
             authenticatedReceipt(packet, receipt, credential_id)) ==
         HistoryReceiptAdmissionCoordinator::SubmitResult::kStarted);
  assert(fixture.admission.pending());
  assert(fixture.history.acknowledgedThrough() == 0);

  // Counter 1 crosses the fresh zero replay bound and therefore needs a
  // durable SecurityStore reservation before application dispatch.
  assert(fixture.security.busy());
  assert(fixture.admission.service() ==
         HistoryReceiptAdmissionCoordinator::ServiceResult::kWaitingReplay);
  assert(fixture.history.acknowledgedThrough() == 0);

  settleSecurity(fixture.security);
  assert(fixture.admission.service() ==
         HistoryReceiptAdmissionCoordinator::ServiceResult::kApplied);
  assert(!fixture.admission.pending());
  assert(fixture.history.acknowledgedThrough() == first.identity);
  assert(fixture.history.deliveredThrough() == 0);
}

void replayDuplicateAndWrongLifetimeNeverReachHistory() {
  Fixture fixture;
  const auto first = allocateRecord(fixture.history, 420000001);
  const auto second = allocateRecord(fixture.history, 420000002);
  appendRecord(fixture.history, first);
  appendRecord(fixture.history, second);

  uint8_t credential_id[kCredentialIdSize]{};
  fixture.credentialId(credential_id);

  const auto first_receipt = oneReceipt(first.identity);
  const auto first_packet = receiptPacket(first_receipt, 1);
  assert(fixture.admission.submitAuthenticatedReceipt(
             authenticatedReceipt(first_packet, first_receipt, credential_id)) ==
         HistoryReceiptAdmissionCoordinator::SubmitResult::kStarted);
  settleSecurity(fixture.security);
  assert(fixture.admission.service() ==
         HistoryReceiptAdmissionCoordinator::ServiceResult::kApplied);
  assert(fixture.history.acknowledgedThrough() == first.identity);

  const auto second_receipt = oneReceipt(second.identity);

  // Same authenticated counter is rejected by SecurityStore and cannot touch
  // the second History identity.
  const auto duplicate_counter_packet = receiptPacket(second_receipt, 1);
  assert(fixture.admission.submitAuthenticatedReceipt(
             authenticatedReceipt(
                 duplicate_counter_packet, second_receipt, credential_id)) ==
         HistoryReceiptAdmissionCoordinator::SubmitResult::kStarted);
  assert(fixture.admission.service() ==
         HistoryReceiptAdmissionCoordinator::ServiceResult::kReplayRejected);
  assert(fixture.history.acknowledgedThrough() == first.identity);

  // A credential-id snapshot from another lifetime is also rejected. The
  // coordinator deliberately does not replace it with currentCredentialId().
  uint8_t wrong_credential_id[kCredentialIdSize]{};
  fixture.credentialId(wrong_credential_id, kCredentialSeed + 1);
  const auto fresh_packet = receiptPacket(second_receipt, 2);
  assert(fixture.admission.submitAuthenticatedReceipt(
             authenticatedReceipt(
                 fresh_packet, second_receipt, wrong_credential_id)) ==
         HistoryReceiptAdmissionCoordinator::SubmitResult::kStarted);
  assert(fixture.admission.service() ==
         HistoryReceiptAdmissionCoordinator::ServiceResult::kReplayRejected);
  assert(fixture.history.acknowledgedThrough() == first.identity);

  // The exact authenticated lifetime and a fresh counter can then apply.
  assert(fixture.admission.submitAuthenticatedReceipt(
             authenticatedReceipt(fresh_packet, second_receipt, credential_id)) ==
         HistoryReceiptAdmissionCoordinator::SubmitResult::kStarted);
  assert(fixture.admission.service() ==
         HistoryReceiptAdmissionCoordinator::ServiceResult::kApplied);
  assert(fixture.history.acknowledgedThrough() == second.identity);
}

void onePendingReceiptOwnsTheReplayResult() {
  Fixture fixture;
  const auto first = allocateRecord(fixture.history, 430000001);
  const auto second = allocateRecord(fixture.history, 430000002);
  appendRecord(fixture.history, first);
  appendRecord(fixture.history, second);

  uint8_t credential_id[kCredentialIdSize]{};
  fixture.credentialId(credential_id);
  const auto first_receipt = oneReceipt(first.identity);
  const auto second_receipt = oneReceipt(second.identity);

  assert(fixture.admission.submitAuthenticatedReceipt(
             authenticatedReceipt(
                 receiptPacket(first_receipt, 1),
                 first_receipt, credential_id)) ==
         HistoryReceiptAdmissionCoordinator::SubmitResult::kStarted);

  assert(fixture.admission.submitAuthenticatedReceipt(
             authenticatedReceipt(
                 receiptPacket(second_receipt, 2),
                 second_receipt, credential_id)) ==
         HistoryReceiptAdmissionCoordinator::SubmitResult::kBusy);
  assert(fixture.history.acknowledgedThrough() == 0);

  settleSecurity(fixture.security);
  assert(fixture.admission.service() ==
         HistoryReceiptAdmissionCoordinator::ServiceResult::kApplied);
  assert(fixture.history.acknowledgedThrough() == first.identity);
}

void acceptedReplayWaitsForBusyHistoryWithoutNewCounter() {
  Fixture fixture;
  const auto first = allocateRecord(fixture.history, 440000001);
  const auto second = allocateRecord(fixture.history, 440000002);
  appendRecord(fixture.history, first);
  appendRecord(fixture.history, second);

  uint8_t credential_id[kCredentialIdSize]{};
  fixture.credentialId(credential_id);
  const auto receipt = oneReceipt(first.identity);
  const auto packet = receiptPacket(receipt, 1);

  assert(fixture.admission.submitAuthenticatedReceipt(
             authenticatedReceipt(packet, receipt, credential_id)) ==
         HistoryReceiptAdmissionCoordinator::SubmitResult::kStarted);
  settleSecurity(fixture.security);

  // Keep HistoryStore busy with an unrelated real append after the security
  // replay decision has become durable.
  const auto third = allocateRecord(fixture.history, 440000003);
  assert(fixture.history.append(third.packet, third.identity));
  assert(fixture.history.busy());

  assert(fixture.admission.service() ==
         HistoryReceiptAdmissionCoordinator::ServiceResult::kWaitingDelivery);
  assert(fixture.admission.pending());
  assert(fixture.history.acknowledgedThrough() == 0);
  assert(fixture.admission.diagnostics().replay_admissions == 1);

  settleHistory(fixture.history);
  bool appended = false;
  assert(fixture.history.takeAppendResult(appended) && appended);

  // No second SecurityStore submission/counter is needed.
  assert(fixture.admission.service() ==
         HistoryReceiptAdmissionCoordinator::ServiceResult::kApplied);
  assert(fixture.history.acknowledgedThrough() == first.identity);
  assert(fixture.admission.diagnostics().replay_admissions == 1);
}

void malformedAuthenticatedPairingFailsBeforeReplayMutation() {
  Fixture fixture;
  const auto first = allocateRecord(fixture.history, 450000001);
  appendRecord(fixture.history, first);

  uint8_t credential_id[kCredentialIdSize]{};
  fixture.credentialId(credential_id);
  const auto receipt = oneReceipt(first.identity);
  auto packet = receiptPacket(receipt, 1);

  const auto security_before = fixture.security.diagnostics();

  // A valid receipt packet length for two IDs paired with a one-ID plaintext
  // is structurally inconsistent. This seam rejects it before SecurityStore.
  packet.ciphertext_len = static_cast<uint8_t>(
      tlp::backendDurableReceiptPlaintextSize(2));
  assert(tlp::validateHistorySecurePacket(packet));
  assert(fixture.admission.submitAuthenticatedReceipt(
             authenticatedReceipt(packet, receipt, credential_id)) ==
         HistoryReceiptAdmissionCoordinator::SubmitResult::
             kInvalidAuthenticatedInput);

  assert(!fixture.admission.pending());
  assert(!fixture.security.busy());
  assert(fixture.history.acknowledgedThrough() == 0);
  assert(fixture.security.diagnostics().a2d_admissions ==
         security_before.a2d_admissions);
  assert(fixture.security.diagnostics().a2d_reservations ==
         security_before.a2d_reservations);
}

void rebootAfterReplayCommitNeedsFreshSecurityCounter() {
  RamFlash<kPageCount> history_flash;
  RamFlash<kFutureSecurityRegionPages> security_flash;
  TestHistoryIncarnationSource incarnation_source;
  HistoryStore history(history_flash, &incarnation_source);
  startHistory(history);

  const auto first = allocateRecord(history, 460000001);
  appendRecord(history, first);

  SecurityStore original_security(security_flash, security_flash);
  startSecurity(original_security);
  HistoryDeliveryCoordinator delivery(history);
  HistoryReceiptAdmissionCoordinator original_admission(
      original_security, delivery);

  uint8_t credential_id[kCredentialIdSize]{};
  fillCredentialId(credential_id);
  const auto receipt = oneReceipt(first.identity);
  const auto original_packet = receiptPacket(receipt, 1);

  assert(original_admission.submitAuthenticatedReceipt(
             authenticatedReceipt(original_packet, receipt, credential_id)) ==
         HistoryReceiptAdmissionCoordinator::SubmitResult::kStarted);
  settleSecurity(original_security);

  // Simulate power loss after the durable SecurityStore replay reservation but
  // before the RAM-only application owner consumes accepted=true.
  assert(history.acknowledgedThrough() == 0);

  SecurityStore recovered_security(security_flash, security_flash);
  assert(recovered_security.begin(
      DeviceIdentity::fromLegacyUint64(kDevice)));
  settleSecurity(recovered_security);
  assert(recovered_security.state() == SecurityState::kProvisioned);

  HistoryReceiptAdmissionCoordinator recovered_admission(
      recovered_security, delivery);

  // The same security counter is burned/rejected after reboot. This preserves
  // anti-replay but cannot advance History by itself.
  assert(recovered_admission.submitAuthenticatedReceipt(
             authenticatedReceipt(original_packet, receipt, credential_id)) ==
         HistoryReceiptAdmissionCoordinator::SubmitResult::kStarted);
  assert(recovered_admission.service() ==
         HistoryReceiptAdmissionCoordinator::ServiceResult::kReplayRejected);
  assert(history.acknowledgedThrough() == 0);

  // Recovery burns the prior eight-counter replay reservation. The backend
  // must reissue the same logical receipt fact under a fresh counter outside
  // that burned range; the History record identity remains unchanged.
  const auto fresh_packet = receiptPacket(receipt, 8);
  assert(recovered_admission.submitAuthenticatedReceipt(
             authenticatedReceipt(fresh_packet, receipt, credential_id)) ==
         HistoryReceiptAdmissionCoordinator::SubmitResult::kStarted);
  settleSecurity(recovered_security);
  assert(recovered_admission.service() ==
         HistoryReceiptAdmissionCoordinator::ServiceResult::kApplied);
  assert(history.acknowledgedThrough() == first.identity);
  assert(history.deliveredThrough() == 0);
}

}  // namespace

int main() {
  replayAcceptanceStrictlyPrecedesDelivery();
  replayDuplicateAndWrongLifetimeNeverReachHistory();
  onePendingReceiptOwnsTheReplayResult();
  acceptedReplayWaitsForBusyHistoryWithoutNewCounter();
  malformedAuthenticatedPairingFailsBeforeReplayMutation();
  rebootAfterReplayCommitNeedsFreshSecurityCounter();
  return 0;
}
