#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gateway_product_startup_guard.h"
#ifdef ORUN_SF5L0_GATEWAY
#include <ble.h>
#endif
#include "security_format.h"

// SF5L0 Tracker/Gateway runtime: one stored record travels Tracker ->
// Gateway, is durably held there, an HMAC custody ACK returns and the Tracker
// durably releases it. Since SF5L0 it also carries step 3A/3B enrollment,
// the drain pacer and the Edge hand-off.
//
// Y2a (the one firmware rak4630_orun): the PUBLIC test credential below (the
// SF3 qualification vector; anybody can derive every key from it) is no
// longer an enrollment or custody path on a device:
//   - ORUN_SF5L0_BENCH_TEST_KEYS (host tests only) adds the test-credential
//     provisioning ("BENCH SETUP") and the Gateway's test-key custody grant;
//   - without it a Tracker can only get a real credential (BENCH PROVISION)
//     and a Gateway grants custody only from the signed OGK1 snapshot or a
//     step 3A RAM grant, so a test-key record is refused;
//   - the test credential id/epoch stay as public facts, so a Tracker that
//     still holds the old test credential can say so (test_credential=yes).
// Until Y2b the Gateway's Edge accept MAC key is still derived from the test
// root (ORUN_SF5L0_HAS_TEST_ROOT, compiled with the Gateway code).
#if defined(ORUN_SF5L0_BENCH_TEST_KEYS) || defined(ORUN_SF5L0_GATEWAY)
#define ORUN_SF5L0_HAS_TEST_ROOT 1
#endif

namespace orun_tlp {

class SecurityStore;

namespace sf5l0_bench {

// Public identifiers of the old test credential (not secret).
extern const uint8_t kTestCredentialId[security_format::kCredentialIdSize];
#ifdef ORUN_SF5L0_HAS_TEST_ROOT
extern const uint8_t kTestRoot[security_format::kKRootSize];
#endif
constexpr uint32_t kTestKeyEpoch = 0x01020304U;
// A freshly migrated v3 SecurityStore starts at floor 0 and the first grant
// receives generation 1. The Gateway test source assumes exactly that.
constexpr uint32_t kTestCustodyFloor = 0U;
constexpr uint32_t kTestGrantGeneration = 1U;

}  // namespace sf5l0_bench

// Tracker side: one explicit USB-started sequence on the REAL SecurityStore:
//   UNPROVISIONED -> commit PUBLIC test credential -> v2->v3 migration ->
//   durable v3 grant for exactly one Gateway ID.
// An already provisioned store is touched only if it holds this same public
// test credential; any other credential is refused, never replaced.
class Sf5l0BenchTestProvisioning final {
 public:
  enum class Step : uint8_t {
    kIdle, kWaitCredential, kMigrate, kWaitMigration,
    kGrant, kWaitGrant, kDone, kFailed
  };

  explicit Sf5l0BenchTestProvisioning(SecurityStore& security)
      : security_(security) {}

  // false = refused immediately; reason() says why.
#ifdef ORUN_SF5L0_BENCH_TEST_KEYS
  bool start(uint64_t gateway_id);  // public test credential (host tests only)
#endif
  // Step 3A: a REAL credential supplied over USB (physical access is the
  // authorization, ADR M7P6 §11). Committed only on an unprovisioned store;
  // a store already holding exactly this credential just (re)grants the
  // Gateway. Any other credential is never overwritten: run the security
  // CLEAN image first. The caller's buffers are not kept.
  bool startWithCredential(uint64_t gateway_id,
                           const uint8_t (&credential_id)[security_format::kCredentialIdSize],
                           uint32_t key_epoch,
                           const uint8_t (&k_root)[security_format::kKRootSize]);
  // Advance at most one step. true when step() changed during this call.
  bool poll();

  Step step() const { return step_; }
  const char* reason() const { return reason_; }
  uint64_t gatewayId() const { return gateway_id_; }

  // Read-only status for the BENCH? report. By the public id and epoch of
  // the old test credential; needs no root.
  static bool holdsTestCredential(const SecurityStore& security);
  static uint8_t activePageVersion(const SecurityStore& security);
  static bool grantFor(const SecurityStore& security, uint64_t gateway_id,
                       uint32_t& generation, uint32_t& floor);
  static bool holdsCredential(
      const SecurityStore& security,
      const uint8_t (&credential_id)[security_format::kCredentialIdSize],
      uint32_t key_epoch, const uint8_t (&k_root)[security_format::kKRootSize]);
  // First granted Gateway ID on a provisioned v3 store (any credential), or 0.
  static uint64_t firstGrantedGateway(const SecurityStore& security);
  // Public key facts of the active credential (never the root).
  static bool credentialInfo(const SecurityStore& security,
                             uint8_t (&credential_id)[security_format::kCredentialIdSize],
                             uint32_t& key_epoch);

 private:
  void fail(const char* why);
  bool begin(uint64_t gateway_id,
             const uint8_t (&credential_id)[security_format::kCredentialIdSize],
             uint32_t key_epoch,
             const uint8_t (&k_root)[security_format::kKRootSize]);

  SecurityStore& security_;
  Step step_ = Step::kIdle;
  uint64_t gateway_id_ = 0U;
  const char* reason_ = "";
};

const char* sf5l0StepName(Sf5l0BenchTestProvisioning::Step step);

// Step 3A USB lines (written by tools/orun_provision.py from the backend
// registry). Pure parsers, host-tested; CRC-32 over the binary fields is a
// typo guard, not authentication (USB physical access is the authority).
enum class Sf5l0LineResult : uint8_t { kOk, kMalformed, kBadCrc };
// "BENCH PROVISION <cred 32> <epoch 8> <root 64> <gateway 16> <crc 8>"
Sf5l0LineResult sf5l0ParseProvisionLine(
    const char* line, size_t length,
    uint8_t (&credential_id)[security_format::kCredentialIdSize],
    uint32_t& key_epoch, uint8_t (&k_root)[security_format::kKRootSize],
    uint64_t& gateway_id);
// "BENCH GRANT <tracker 16> <epoch 8> <floor 8> <generation 8> <key 64> <crc 8>"
Sf5l0LineResult sf5l0ParseGrantLine(const char* line, size_t length,
                                    uint64_t& tracker_id, uint32_t& key_epoch,
                                    uint32_t& custody_floor,
                                    uint32_t& grant_generation,
                                    uint8_t (&scoped_key)[32]);

// SF5L2: bench pacing for draining the Tracker backlog with the one-record
// pilot. Decides only WHEN the pilot may be re-armed; selection, sealing,
// caching and release stay with their existing owners.
//  - after a durable release: next record after a short gap;
//  - no release within ack_wait: same record again after a back-off;
//  - token bucket on PRODUCT_SECURE airtime (rate duty_ppm, depth burst_ms)
//    bounds this sender to (burst + duty x 1 h) of airtime in any hour.
// v1 POSITION / HISTORY_SECURE airtime is NOT counted here.
class Sf5l0DrainPacer final {
 public:
  struct Config {
    uint32_t gap_after_release_ms = 3000U;
    uint32_t ack_wait_ms = 15000U;       // longer than the 10 s RX window
    uint32_t retry_backoff_ms = 30000U;  // minimum after a missed ACK
    uint32_t retry_jitter_ms = 60000U;   // per-Tracker 0..60 s extra delay
    uint32_t airtime_ms = 2300U;         // SF11 / 125 kHz, 100-byte frame
    uint32_t duty_ppm = 60000U;          // 6 % long-run, product frames only
    uint32_t burst_ms = 60000U;          // bucket depth
  };
  struct Stats {
    uint32_t sent = 0U;
    uint32_t released = 0U;
    uint32_t ack_timeouts = 0U;
    uint32_t duty_waits = 0U;
  };

  Sf5l0DrainPacer() : config_(Config()) {}
  explicit Sf5l0DrainPacer(const Config& config) : config_(config) {}

  // device_id only seeds non-cryptographic retry timing; not a security key.
  void begin(uint32_t now_ms, uint64_t device_id = 0U);
  void setEnabled(bool enabled) { enabled_ = enabled; }
  bool enabled() const { return enabled_; }
  // true: the pilot may be re-armed now (caller then calls rearm()).
  bool mayRearm(uint32_t now_ms);
  void onSent(uint32_t now_ms);
  void onReleased(uint32_t now_ms);

  bool awaitingAck() const { return awaiting_ack_; }
  uint32_t tokensMs() const { return static_cast<uint32_t>(tokens_us_ / 1000U); }
  const Stats& stats() const { return stats_; }
  const Config& config() const { return config_; }

 private:
  void refill(uint32_t now_ms);
  uint32_t nextRandom();

  Config config_;
  Stats stats_{};
  bool enabled_ = true;
  bool awaiting_ack_ = false;
  bool duty_wait_counted_ = false;
  uint32_t sent_at_ms_ = 0U;
  uint32_t next_allowed_ms_ = 0U;
  uint32_t last_refill_ms_ = 0U;
  uint32_t random_state_ = 0x6D2B79F5U;
  uint64_t tokens_us_ = 0U;  // airtime budget in microseconds
};

// Gateway side: a durable-grant source that DERIVES the scoped custody ACK
// key from the public test root. Real Gateways must never hold a Tracker
// root; this exists only because the test root is public by construction.
class Sf5l0BenchGrantSource final : public GatewayDurableGrantSource {
 public:
  explicit Sf5l0BenchGrantSource(uint64_t local_gateway_id)
      : gateway_id_(local_gateway_id) {}

  bool verifiedForLocalGateway(uint64_t gateway_id) const override;
  bool activeGrant(uint64_t tracker_id, uint32_t key_epoch,
                   GatewayCustodyAckScope& scope,
                   uint8_t (&scoped_key)[32]) override;

  uint32_t grantsIssued() const { return grants_issued_; }
  uint32_t grantsRefused() const { return grants_refused_; }

  // Step 3A (bench interim until the signed, persistent grant snapshot of
  // step 3B): a scoped custody ACK key for one (Tracker, epoch), derived by
  // the backend from that Tracker's real credential and written over USB.
  // RAM only: gone after a reboot (the Tracker keeps its records until a
  // Gateway can ACK again). A real grant always wins over the test key.
  static constexpr uint8_t kRamGrants = 8U;
  enum class AddResult : uint8_t { kAdded, kReplaced, kFull, kInvalid };
  AddResult addGrant(uint64_t tracker_id, uint32_t key_epoch,
                     uint32_t custody_floor, uint32_t grant_generation,
                     const uint8_t (&scoped_key)[32]);
  uint8_t ramGrants() const;

  // Step 3B: the verified, signed OGK1 snapshot (GatewayGrantSnapshotReader)
  // is asked first; then the RAM table. Only host tests
  // (ORUN_SF5L0_BENCH_TEST_KEYS) fall back to the public test key; a device
  // refuses a test-key Tracker (Y2a).
  void setSigned(GatewayDurableGrantSource* signed_source) {
    signed_ = signed_source;
  }
  uint32_t signedIssued() const { return signed_issued_; }

 private:
  struct RamGrant {
    bool used = false;
    GatewayCustodyAckScope scope{};
    uint8_t key[32]{};
  };
  RamGrant ram_[kRamGrants]{};
  GatewayDurableGrantSource* signed_ = nullptr;
  uint32_t signed_issued_ = 0U;
  uint64_t gateway_id_ = 0U;
  uint32_t grants_issued_ = 0U;
  uint32_t grants_refused_ = 0U;
};

class RadioManager;
class ProductCustodyStore;

// Read-only walk over every record the Gateway currently holds (OCQ2 HELD,
// confirmed through ProductCustodyStore::readConfirmedHeld), oldest page
// generation first, then slot order. Never writes, hands off or deletes.
struct Sf5l0HeldRecord {
  uint16_t page = 0U;
  uint16_t slot = 0U;
  uint64_t generation = 0U;
  uint8_t size = 0U;
  uint8_t object[100]{};
};
class Sf5l0HeldVisitor {
 public:
  virtual ~Sf5l0HeldVisitor() = default;
  virtual void held(const Sf5l0HeldRecord& record) = 0;
};
// Returns the number of records visited; false-free: unreadable or
// non-active pages are skipped, never repaired.
uint16_t sf5l0ForEachHeld(const FlashBackend& product_port,
                          const ProductCustodyStore& store,
                          uint16_t page_count, Sf5l0HeldVisitor& visitor);

// Gateway bench image composition (src/sf5l0_bench_gateway.cpp, compiled
// only with ORUN_SF5L0_GATEWAY). Kept out of main.cpp so the default image's
// source-ownership contracts stay exactly as they are.
// SF5L5 bench Edge hand-off. The PC tool, after its ledger has durably
// committed a record (SQLite, synchronous=FULL), sends
//   EDGE ACCEPT v1 page=<P> slot=<S> gen=<16 hex> mac=<64 hex>
// mac = HMAC-SHA256(K_edge, M), M = 102 bytes: "ORUN-EDGE-ACCEPT-v1" |
//        gateway id (8, BE) | page (2, BE) | slot (2, BE) | generation (8, BE)
//        | SHA-256(object) | zero padding
// K_edge = HKDF-SHA256(salt = test credential id, ikm = test root, info = 57
//          bytes: "ORUN-SF5L0-BENCH-EDGE-ACCEPT-v1" | gateway id (8, BE) |
//          zero padding)  (fixed sizes: the CC310 primitives' only lengths)
// BENCH ONLY: K_edge comes from the PUBLIC test credential, so this proves
// the mechanism (exact-object binding, idempotence, reclaim), NOT that the
// accept came from an enrolled Edge. Production needs an enrolled Edge key.
class Sf5l0EdgeHandoff final {
 public:
  enum class Reply : uint8_t {
    kPending,      // marker write started; the outcome is a later Event
    kAlready,      // repeat of an earlier accept (idempotent)
    kUnknown,      // no such held object here (e.g. reclaimed, other bytes)
    kBusy,         // store busy; the Edge retries
    kBadMac, kMalformed, kRejected
  };
  struct Event {
    enum class Kind : uint8_t { kNone, kHandedOff, kReclaimStarted,
                                kReclaimed } kind = Kind::kNone;
    bool ok = false;
    uint16_t page = 0;
    uint16_t slot = 0;
  };
  Sf5l0EdgeHandoff(ProductCustodyStore& store, uint64_t gateway_id)
      : store_(store), gateway_id_(gateway_id) {}
  Reply accept(const char* line, size_t length, uint16_t& page,
               uint16_t& slot);
  // Same rule for an already parsed accept (BLE Edge link).
  Reply accept(uint16_t page, uint16_t slot, uint64_t generation,
               const uint8_t (&mac)[32]);
  // Loop: drives the store's maintenance and erases fully handed-off pages.
  Event service();
  uint32_t handedOff() const { return handed_off_; }
  uint32_t reclaimed() const { return reclaimed_; }

  static bool mac(uint64_t gateway_id, uint16_t page, uint16_t slot,
                  uint64_t generation, const uint8_t* object, size_t size,
                  uint8_t (&out)[32]);

 private:
  ProductCustodyStore& store_;
  uint64_t gateway_id_;
  bool pending_ = false;
  bool check_reclaim_ = true;  // once at boot, then after each hand-off
  uint32_t handed_off_ = 0;
  uint32_t reclaimed_ = 0;
};

// SF5L6 bench BLE Edge session: the Gateway side of edge_link.h for ONE
// connected Edge. Pull model, no queue of records: a (generation, slot)
// cursor walks the records the store holds right now, so the first pass is
// the full dump, later records stream as they arrive and handed-off records
// are skipped by construction. Same accept rule and owner as USB.
class Sf5l0EdgeLinkSession final {
 public:
  Sf5l0EdgeLinkSession(const FlashBackend& port, ProductCustodyStore& store,
                       Sf5l0EdgeHandoff& handoff, uint64_t gateway_id,
                       uint16_t page_count)
      : port_(port), store_(store), handoff_(handoff),
        gateway_id_(gateway_id), page_count_(page_count) {}
  void begin();                     // new connection: HELLO, then full pass
  void end() { active_ = false; }
  bool active() const { return active_; }
  // A complete Edge -> Gateway logical message.
  void onMessage(const uint8_t* message, size_t size);
  // An accept this session submitted has finished (Sf5l0EdgeHandoff event).
  void onAcceptDone(uint16_t page, uint16_t slot, bool ok);
  void onReclaimed(uint16_t page, bool ok);
  // A new record was taken into custody; link facts are that frame's RX.
  void onNewRecord(uint16_t page, uint16_t slot, uint64_t generation,
                   bool has_link, int16_t rssi, int8_t snr);
  // Next logical message to send, if any.
  bool next(uint8_t* out, size_t capacity, size_t& length);
  bool acceptPending() const { return accept_pending_; }

 private:
  struct Result { uint16_t page; uint16_t slot; uint8_t code; bool reclaim; };
  bool pushResult(const Result& r);
  bool nextHeld(uint8_t* out, size_t capacity, size_t& length);
  bool nextPriority(uint8_t* out, size_t capacity, size_t& length);

  const FlashBackend& port_;
  ProductCustodyStore& store_;
  Sf5l0EdgeHandoff& handoff_;
  uint64_t gateway_id_;
  uint16_t page_count_;
  bool active_ = false;
  bool hello_pending_ = false;
  bool priority_pending_ = false;   // one newest custody preview, not DUMP census
  bool priority_sent_since_history_ = false; // avoid starving the chronological scan
  bool scan_pending_ = false;   // records may exist after the cursor
  bool dump_end_pending_ = false;
  bool cursor_valid_ = false;
  uint64_t cursor_generation_ = 0;
  uint16_t cursor_slot_ = 0;
  uint16_t sent_this_pass_ = 0;
  bool accept_pending_ = false;
  uint16_t accept_page_ = 0;
  uint16_t accept_slot_ = 0;
  Result results_[6]{};
  uint8_t results_n_ = 0;
  bool link_valid_ = false;
  uint16_t link_page_ = 0;
  uint16_t link_slot_ = 0;
  uint64_t link_generation_ = 0;
  int16_t link_rssi_ = 0;
  int8_t link_snr_ = 0;
};

namespace sf5l0_gateway {
// setup(), before Bluefruit.begin(): census + OCQ2 mount via the startup guard.
// Step 3B: security_port is the Gateway-owned security region (the Tracker
// SecurityStore is NOT mounted on this image); its signed OGK1 snapshot is
// verified here, while the synchronous flash path is still available.
void begin(FlashBackend& product_port, FlashBackend& security_port,
           RadioManager& radio, uint64_t gateway_id,
           bool observation_store_mounted);
// loop(), after RadioManager::update() and only while not transmitting.
void service(bool observation_store_mounted);
// "BENCH?"
void print();
// "EDGE ACCEPT v1 ...": one authenticated (bench key) Edge durable accept.
void edgeAccept(const char* line, size_t length);
// Step 3A: "BENCH GRANT <tracker 16 hex> <epoch 8 hex> <floor 8 hex>
// <generation 8 hex> <key 64 hex> <crc 8 hex>" (crc = CRC-32 of the 52 binary
// bytes, typo guard). Installs a RAM-only scoped custody ACK key.
void grantLine(const char* line, size_t length);
// Step 3B: "BENCH OGK1 BEGIN | DATA <off> <hex> | COMMIT <crc> | ABORT":
// upload of a backend-signed grant snapshot (step3b_gateway_enrollment.h).
// The new snapshot is used after the automatic reboot.
void ogk1Line(const char* line, size_t length);
// SF5L6 BLE Edge service (edge_link.h). beginBle(): after Bluefruit.begin(),
// before advertising starts. onBleEvent(): from the Bluefruit event callback
// (BLE task: counter only). pollBle(): loop, with the sampled connection.
#ifdef ORUN_SF5L0_GATEWAY  // device build only (SoftDevice types)
bool beginBle();
void onBleEvent(ble_evt_t* evt);
void pollBle(bool connected, uint16_t connection_handle, bool disconnect_event,
             uint32_t now);
#endif
// "BENCH GW ERASE PRODUCT CONFIRM": wipe this bench board's product region
// when it is not a READY custody store. include_held=true ("BENCH GW ERASE
// HELD CONFIRM") also wipes a READY store: bench-only stand-in for the
// missing authenticated Edge accept, used once the PC ledger has the records.
void startErase(bool include_held);
// "BENCH GW DUMP": print every held record as an EDGE HELD line.
void dump();
// 6: LoRa command lines over the Bluetooth Edge link. A phone bridge sends
// the same "APP LORA ..." lines as USB (handled by `handler`); every "LORA ..."
// line the Gateway prints is also sent to a connected phone bridge.
void setLoraLineHandler(bool (*handler)(const char* line, size_t length));
void edgeLoraLine(const char* line);
}  // namespace sf5l0_gateway

}  // namespace orun_tlp
