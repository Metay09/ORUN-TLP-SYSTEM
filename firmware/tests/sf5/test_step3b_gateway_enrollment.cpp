// Step 3B: backend-signed OGK1 grant snapshot installed over USB into the
// Gateway-owned security region. Host only: OpenSSL Ed25519 stands in for the
// pinned CC310 verifier (same 32-byte key / 64-byte signature / 1824-byte
// message contract as SF5K3AA), fake NOR stands in for the security region.
//
// The snapshots are built and signed by backend/orun_credentials.py (vectors
// file, argv[1]); this test only replays the exact USB lines the PC tool
// sends, so the Python builder and the firmware reader/installer are checked
// against each other byte for byte.
//
// Pins:
//  - first snapshot on a blank region; strictly higher generation replaces
//    it on the OTHER page; revocation by a newer snapshot without the Tracker;
//  - wrong Gateway, wrong issuer, tampered bytes (with and without a fixed
//    CRC), not-newer generations, foreign (ORS1) region: refused before any
//    flash mutation, and the boot snapshot stays usable;
//  - the boot snapshot is denied before the first mutation; async flash
//    completions are polled, never resubmitted; a flash error latches FAULT;
//    a torn page is never accepted at the next boot;
//  - the bench grant chain asks the signed snapshot first, then RAM, then
//    the public test key.
#include <openssl/evp.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "gateway_grant_snapshot_reader.h"
#include "observation_store_format.h"
#include "sf5l0_bench.h"
#include "step3b_gateway_enrollment.h"

using namespace orun_tlp;
namespace fmt = orun_tlp::gateway_grant_format;
using Reply = Step3bGrantInstaller::Reply;
using State = Step3bGrantInstaller::State;
using RStatus = GatewayGrantSnapshotReader::Status;

namespace {

// ---------------------------------------------------------------- vectors --
struct Grant {
  uint64_t tracker = 0U;
  uint32_t epoch = 0U, floor = 0U, generation = 0U;
  uint8_t key[32]{};
};
struct Snapshot {
  std::vector<std::string> lines;
  std::vector<Grant> grants;
  uint64_t generation = 0U;
};
uint8_t issuer_public[32]{};
uint8_t other_issuer_public[32]{};
uint64_t gateway = 0U;
std::map<std::string, Snapshot> snaps;

uint64_t hexU64(const std::string& s) { return strtoull(s.c_str(), nullptr, 16); }
void hexInto(const std::string& s, uint8_t* out, size_t n) {
  assert(s.size() == 2U * n);
  for (size_t i = 0; i < n; ++i)
    out[i] = static_cast<uint8_t>(strtoul(s.substr(2U * i, 2U).c_str(), nullptr, 16));
}

void loadVectors(const char* path) {
  FILE* f = fopen(path, "r");
  assert(f != nullptr);
  char buf[512];
  std::string current;
  while (fgets(buf, sizeof(buf), f)) {
    std::string line(buf);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.empty()) continue;
    char a[64]{}, b[200]{}, c[64]{}, d[64]{}, e[64]{}, g[64]{}, h[80]{};
    if (line.rfind("BENCH OGK1 ", 0) == 0) {
      assert(!current.empty());
      assert(line.size() < 160U);  // fits the bench command buffer
      snaps[current].lines.push_back(line);
    } else if (sscanf(line.c_str(), "issuer %64s", b) == 1) {
      hexInto(b, issuer_public, 32);
    } else if (sscanf(line.c_str(), "other_issuer %64s", b) == 1) {
      hexInto(b, other_issuer_public, 32);
    } else if (sscanf(line.c_str(), "gateway %16s", b) == 1) {
      gateway = hexU64(b);
    } else if (sscanf(line.c_str(), "snap %63s %16s", a, b) == 2) {
      current = a;
      snaps[current].generation = hexU64(b);
    } else if (sscanf(line.c_str(), "grant %63s %16s %8s %8s %8s %64s", a, c, d, e, g, h) == 6) {
      Grant gr;
      gr.tracker = hexU64(c);
      gr.epoch = static_cast<uint32_t>(hexU64(d));
      gr.floor = static_cast<uint32_t>(hexU64(e));
      gr.generation = static_cast<uint32_t>(hexU64(g));
      hexInto(h, gr.key, 32);
      snaps[a].grants.push_back(gr);
    } else {
      fprintf(stderr, "bad vector line: %s\n", line.c_str());
      assert(false);
    }
  }
  fclose(f);
  assert(gateway != 0U && issuer_public[0] | issuer_public[1]);
}

// Image <-> lines, for tampering tests (the CRC of the lines is recomputed).
std::vector<uint8_t> imageOf(const Snapshot& s) {
  std::vector<uint8_t> image;
  for (const auto& l : s.lines) {
    if (l.rfind("BENCH OGK1 DATA ", 0) != 0) continue;
    const std::string hex = l.substr(21);
    for (size_t i = 0; i < hex.size(); i += 2U)
      image.push_back(static_cast<uint8_t>(strtoul(hex.substr(i, 2U).c_str(), nullptr, 16)));
  }
  assert(image.size() == fmt::kStoredBytes);
  return image;
}
std::vector<std::string> linesOf(const std::vector<uint8_t>& image) {
  std::vector<std::string> out{"BENCH OGK1 BEGIN"};
  char buf[200];
  for (size_t off = 0; off < image.size(); off += 64U) {
    int n = snprintf(buf, sizeof(buf), "BENCH OGK1 DATA %04X ", unsigned(off));
    for (size_t i = off; i < off + 64U && i < image.size(); ++i)
      n += snprintf(buf + n, sizeof(buf) - size_t(n), "%02X", image[i]);
    out.push_back(buf);
  }
  snprintf(buf, sizeof(buf), "BENCH OGK1 COMMIT %08X",
           unsigned(observation_store_format::crc32(image.data(), image.size())));
  out.push_back(buf);
  return out;
}

// ------------------------------------------------------------- verifier --
struct OpenSslEd25519 final : GatewayEnrollmentProofVerifier {
  uint8_t key[32]{};
  mutable unsigned calls = 0U;
  explicit OpenSslEd25519(const uint8_t (&k)[32]) { memcpy(key, k, sizeof(key)); }
  bool verify(const uint8_t* data, size_t length,
              const uint8_t (&proof)[fmt::kProofBytes]) const override {
    ++calls;
    if (!data || length != fmt::kPayloadBytes) return false;
    EVP_PKEY* pk = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key, sizeof(key));
    if (!pk) return false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pk) == 1 &&
              EVP_DigestVerify(ctx, proof, sizeof(proof), data, length) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pk);
    return ok;
  }
};

// ---------------------------------------------------------------- fake NOR --
// 2 x 4 KiB security region. Programming only clears bits and (like the
// FlashMutationGate security port) refuses a non-erased destination.
struct Nor final : FlashBackend {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(2U * fmt::kPageBytes, 0xFFU);
  bool async = false;
  bool unreconciled = false;
  int fail_at = -1;       // mutation index that fails
  int power_cut_at = -1;  // from this mutation on nothing happens (kFailed)
  int disturb_at = -1;    // after this mutation, an earlier byte loses a bit
  uint32_t disturb_offset = 0U;
  int mutations = 0;
  bool pending = false;
  bool resubmitted = false;
  std::vector<uint8_t> pending_data;
  uint32_t pending_offset = 0U;
  int pending_page = -1;

  bool begin() override { return true; }
  bool read(uint32_t offset, void* out, size_t n) const override {
    if (!out || offset > bytes.size() || n > bytes.size() - offset) return false;
    memcpy(out, bytes.data() + offset, n);
    return true;
  }
  bool hasUnreconciledMutation() const override { return unreconciled; }
  FlashOpResult apply() {
    if (pending_page >= 0) {
      memset(bytes.data() + uint32_t(pending_page) * fmt::kPageBytes, 0xFF, fmt::kPageBytes);
    } else {
      for (size_t i = 0; i < pending_data.size(); ++i)
        bytes[pending_offset + i] &= pending_data[i];
    }
    if (mutations - 1 == disturb_at) bytes[disturb_offset] ^= 0x01U;
    return FlashOpResult::kDone;
  }
  FlashOpResult start(int page, uint32_t offset, const void* data, size_t n) {
    if (pending) {
      resubmitted = true;
      return FlashOpResult::kFailed;
    }
    const int index = mutations++;
    if (power_cut_at >= 0 && index >= power_cut_at) return FlashOpResult::kFailed;
    if (index == fail_at) return FlashOpResult::kFailed;
    pending_page = page;
    pending_offset = offset;
    pending_data.assign(static_cast<const uint8_t*>(data),
                        static_cast<const uint8_t*>(data) + n);
    if (async) {
      pending = true;
      return FlashOpResult::kPending;
    }
    return apply();
  }
  FlashOpResult program(uint32_t offset, const void* data, size_t n) override {
    if (!data || n == 0U || (offset & 3U) || (n & 3U) || n > 96U ||
        offset + n > bytes.size() || n > fmt::kPageBytes - offset % fmt::kPageBytes)
      return FlashOpResult::kFailed;
    for (size_t i = 0; i < n; ++i)
      if (bytes[offset + i] != 0xFFU) return FlashOpResult::kFailed;
    return start(-1, offset, data, n);
  }
  FlashOpResult erasePage(uint32_t page) override {
    if (page >= 2U) return FlashOpResult::kFailed;
    return start(int(page), 0U, "", 0U);
  }
  FlashOpResult pollPending() override {
    if (!pending) return FlashOpResult::kFailed;
    pending = false;
    return apply();
  }
};

// ---------------------------------------------------------------- helpers --
struct Boot {
  OpenSslEd25519 verifier{issuer_public};
  GatewayGrantSnapshotReader reader;
  Step3bGrantInstaller installer;
  RStatus status;
  explicit Boot(Nor& nor)
      : reader(nor, verifier), installer(nor, verifier, gateway),
        status(reader.begin(gateway, true, false)) {}
};

Reply upload(Boot& boot, const std::vector<std::string>& lines) {
  Reply last = Reply::kNotMine;
  for (const auto& l : lines) {
    last = boot.installer.handleLine(l.data(), l.size(), boot.reader);
    if (l.rfind("BENCH OGK1 COMMIT", 0) == 0) return last;
    if (last != Reply::kOk) return last;
  }
  return last;
}
State drive(Boot& boot, Nor& nor) {
  for (int i = 0; i < 400; ++i) {
    const State s = boot.installer.poll();
    if (s != State::kWriting) return s;
    (void)nor;
  }
  return boot.installer.state();
}
void install(Nor& nor, const char* name, uint16_t expected_page) {
  Boot boot(nor);
  assert(boot.status == RStatus::kReady || boot.status == RStatus::kEmpty);
  assert(upload(boot, snaps.at(name).lines) == Reply::kWriteStarted);
  assert(boot.installer.destinationPage() == expected_page);
  assert(boot.installer.candidateGeneration() == snaps.at(name).generation);
  // The boot snapshot no longer authorizes anything during the write.
  assert(!boot.reader.verifiedForLocalGateway(gateway));
  assert(drive(boot, nor) == State::kDone);
  assert(!nor.resubmitted);
}
void expectGrants(Nor& nor, const char* name) {
  Boot boot(nor);
  assert(boot.status == RStatus::kReady);
  assert(boot.reader.activeGeneration() == snaps.at(name).generation);
  for (const Grant& g : snaps.at(name).grants) {
    GatewayCustodyAckScope scope{};
    uint8_t key[32]{};
    assert(boot.reader.activeGrant(g.tracker, g.epoch, scope, key));
    assert(scope.gateway_device_id == gateway && scope.custody_floor == g.floor &&
           scope.grant_generation == g.generation);
    assert(memcmp(key, g.key, sizeof(key)) == 0);
  }
}

// ------------------------------------------------------------------- tests --
void testStagedView() {
  uint8_t image[fmt::kStoredBytes];
  memset(image, 0xA5, sizeof(image));
  Step3bStagedSnapshotFlash view(image);
  uint8_t out[8]{};
  assert(view.begin() && view.read(fmt::kStoredBytes - 4U, out, 8U));
  assert(out[0] == 0xA5 && out[3] == 0xA5 && out[4] == 0xFF && out[7] == 0xFF);
  assert(view.read(fmt::kPageBytes, out, 8U) && out[0] == 0xFF);
  assert(!view.read(2U * fmt::kPageBytes - 4U, out, 8U));
  assert(view.program(0U, out, 4U) == FlashOpResult::kFailed);
  assert(view.erasePage(0U) == FlashOpResult::kFailed);
  Step3bStagedSnapshotFlash none(nullptr);
  assert(!none.begin());
}

void testLines() {
  Nor nor;
  Boot boot(nor);
  assert(boot.status == RStatus::kEmpty);
  auto send = [&](const char* l) { return boot.installer.handleLine(l, strlen(l), boot.reader); };
  assert(send("BENCH GRANT x") == Reply::kNotMine);
  assert(send("BENCH OGK1 DATA 0000 00") == Reply::kNotReceiving);
  assert(send("BENCH OGK1 COMMIT 00000000") == Reply::kNotReceiving);
  assert(send("BENCH OGK1 BEGIN") == Reply::kOk);
  assert(send("BENCH OGK1 DATA 0004 00") == Reply::kBadOffset);     // must be sequential
  assert(send("BENCH OGK1 DATA 0000 0g") == Reply::kMalformed);     // not hex
  assert(send("BENCH OGK1 DATA 0000 ab") == Reply::kMalformed);     // lowercase refused
  assert(send("BENCH OGK1 DATA 0000 0") == Reply::kMalformed);      // odd length
  assert(send("BENCH OGK1 DATA 000 00") == Reply::kMalformed);      // offset width
  assert(send("BENCH OGK1 DATA 0000") == Reply::kMalformed);
  assert(send("BENCH OGK1 NOPE") == Reply::kMalformed);
  assert(send("BENCH OGK1 DATA 0000 00112233\r") == Reply::kOk);
  assert(boot.installer.received() == 4U);
  assert(send("BENCH OGK1 COMMIT 1234") == Reply::kMalformed);
  assert(send("BENCH OGK1 COMMIT 00000000") == Reply::kIncomplete);
  // A refused COMMIT drops the candidate.
  assert(boot.installer.state() == State::kIdle && boot.installer.received() == 0U);
  assert(send("BENCH OGK1 BEGIN") == Reply::kOk);
  assert(send("BENCH OGK1 ABORT") == Reply::kAborted);
  assert(boot.installer.state() == State::kIdle);
  // Too much data is refused, never written past the staging buffer.
  assert(send("BENCH OGK1 BEGIN") == Reply::kOk);
  const auto image = imageOf(snaps.at("g1"));
  auto lines = linesOf(image);
  for (size_t i = 1; i + 1 < lines.size(); ++i)
    assert(boot.installer.handleLine(lines[i].data(), lines[i].size(), boot.reader) == Reply::kOk);
  assert(send("BENCH OGK1 DATA 0768 00000000") == Reply::kBadOffset);
  assert(nor.mutations == 0);
}

void testFirstSnapshotThenRotationThenRevocation(bool async) {
  Nor nor;
  nor.async = async;
  install(nor, "g1", 0U);
  expectGrants(nor, "g1");
  const std::vector<uint8_t> page0(nor.bytes.begin(), nor.bytes.begin() + fmt::kPageBytes);
  const auto image = imageOf(snaps.at("g1"));
  assert(memcmp(page0.data(), image.data(), image.size()) == 0);
  for (size_t i = fmt::kStoredBytes; i < fmt::kPageBytes; ++i) assert(page0[i] == 0xFF);

  install(nor, "g2", 1U);  // the other page; the old one is left intact
  assert(memcmp(nor.bytes.data(), page0.data(), page0.size()) == 0);
  expectGrants(nor, "g2");

  install(nor, "g3_revoke", 0U);
  {
    Boot boot(nor);
    assert(boot.status == RStatus::kReady && boot.reader.activeGeneration() == 3U);
    GatewayCustodyAckScope scope{};
    uint8_t key[32]{};
    for (const Grant& g : snaps.at("g2").grants)
      assert(!boot.reader.activeGrant(g.tracker, g.epoch, scope, key));
  }
  assert(!nor.resubmitted);
}

void expectRefused(Nor& nor, const std::vector<std::string>& lines, Reply expected) {
  const std::vector<uint8_t> before = nor.bytes;
  Boot boot(nor);
  const RStatus status = boot.status;
  const uint64_t generation = boot.reader.activeGeneration();
  const int mutations = nor.mutations;
  assert(upload(boot, lines) == expected);
  assert(boot.installer.state() == State::kIdle);
  assert(boot.installer.poll() == State::kIdle);
  assert(nor.mutations == mutations && nor.bytes == before);
  // Still the same authority as before the refused upload.
  assert(boot.reader.status() == status);
  if (status == RStatus::kReady)
    assert(boot.reader.verifiedForLocalGateway(gateway) &&
           boot.reader.activeGeneration() == generation);
}

void testRefusals() {
  Nor nor;
  install(nor, "g1", 0U);
  install(nor, "g2", 1U);
  expectRefused(nor, snaps.at("g1").lines, Reply::kNotNewer);   // rollback
  expectRefused(nor, snaps.at("g2").lines, Reply::kNotNewer);   // same generation
  expectRefused(nor, snaps.at("wrong_gateway").lines, Reply::kWrongGateway);
  expectRefused(nor, snaps.at("wrong_issuer").lines, Reply::kNotAuthentic);

  auto image = imageOf(snaps.at("g5"));
  auto lines = snaps.at("g5").lines;
  lines.back()[lines.back().size() - 1] ^= 1;                  // transport typo
  expectRefused(nor, lines, Reply::kBadCrc);

  auto tampered = image;
  tampered[fmt::kHeaderBytes + 30U] ^= 0x01U;                   // a key byte
  expectRefused(nor, linesOf(tampered), Reply::kNotAuthentic);   // OGK1 CRC now wrong
  const uint32_t crc = observation_store_format::crc32(tampered.data(), fmt::kCrcOffset);
  tampered[fmt::kCrcOffset] = uint8_t(crc >> 24); tampered[fmt::kCrcOffset + 1] = uint8_t(crc >> 16);
  tampered[fmt::kCrcOffset + 2] = uint8_t(crc >> 8); tampered[fmt::kCrcOffset + 3] = uint8_t(crc);
  expectRefused(nor, linesOf(tampered), Reply::kNotAuthentic);   // signature fails

  auto uncommitted = image;
  uncommitted[fmt::kCommitOffset] = 0xFF;                       // commit word not 0
  expectRefused(nor, linesOf(uncommitted), Reply::kNotAuthentic);

  // The refusals left the region usable: a newer genuine snapshot installs.
  install(nor, "g5", 0U);
  expectGrants(nor, "g5");
}

void testForeignRegionNotOverwritten() {
  Nor nor;
  nor.bytes[0] = 'O'; nor.bytes[1] = 'R'; nor.bytes[2] = 'S'; nor.bytes[3] = '1';
  {
    Boot boot(nor);
    assert(boot.status == RStatus::kRejected);
  }
  expectRefused(nor, snaps.at("g1").lines, Reply::kRegionNotReady);
}

void testFlashFaultAndTornPage() {
  for (int cut = 0; cut < 32; cut += 3) {
    Nor nor;
    install(nor, "g1", 0U);
    nor.fail_at = nor.mutations + cut;  // erase, then 30 body chunks, then commit
    Boot boot(nor);
    assert(upload(boot, snaps.at("g2").lines) == Reply::kWriteStarted);
    assert(drive(boot, nor) == State::kFault);
    assert(!boot.reader.verifiedForLocalGateway(gateway));  // denied, latched
    const char* again = "BENCH OGK1 BEGIN";
    assert(boot.installer.handleLine(again, strlen(again), boot.reader) == Reply::kBusy);
    // Next boot: the half-written page is never trusted (documented: CLEAN
    // and upload again). An erase-only failure leaves g1 alone and valid.
    Boot next(nor);
    if (cut == 0) {
      assert(next.status == RStatus::kReady && next.reader.activeGeneration() == 1U);
    } else {
      assert(next.status == RStatus::kRejected);
      GatewayCustodyAckScope scope{};
      uint8_t key[32]{};
      assert(!next.reader.activeGrant(snaps.at("g1").grants[0].tracker,
                                      snaps.at("g1").grants[0].epoch, scope, key));
    }
  }
  {
    // A byte of an earlier, already verified chunk changes before the commit
    // marker: the full pre-commit readback refuses to commit.
    Nor nor;
    install(nor, "g1", 0U);
    nor.disturb_at = nor.mutations + 30;           // after the last body chunk
    nor.disturb_offset = fmt::kPageBytes + 100U;   // page 1, chunk 1
    Boot boot(nor);
    assert(upload(boot, snaps.at("g2").lines) == Reply::kWriteStarted);
    assert(drive(boot, nor) == State::kFault);
    assert(nor.mutations == 32 + 31);              // erase + body; no commit word
    Boot next(nor);
    assert(next.status == RStatus::kRejected);
  }
  // Power cut after the body, before the commit word: still never trusted.
  Nor nor;
  install(nor, "g1", 0U);
  nor.power_cut_at = nor.mutations + 31;  // erase + 30 body chunks done
  Boot boot(nor);
  assert(upload(boot, snaps.at("g2").lines) == Reply::kWriteStarted);
  assert(drive(boot, nor) == State::kFault);
  Boot next(nor);
  assert(next.status == RStatus::kRejected);
}

void testUnreconciledMutationFaults() {
  Nor nor;
  nor.async = true;
  Boot boot(nor);
  assert(upload(boot, snaps.at("g1").lines) == Reply::kWriteStarted);
  assert(boot.installer.poll() == State::kWriting);   // erase pending
  nor.unreconciled = true;
  assert(boot.installer.poll() == State::kFault);
  nor.unreconciled = false;
  Nor blocked;
  blocked.unreconciled = true;
  Boot b2(blocked);
  assert(b2.status == RStatus::kUnavailable);
  assert(upload(b2, snaps.at("g1").lines) == Reply::kRegionNotReady);
}

void testGrantChain() {
  Nor nor;
  install(nor, "g2", 0U);
  Boot boot(nor);
  assert(boot.status == RStatus::kReady);
  Sf5l0BenchGrantSource chain(gateway);
  const Grant& g = snaps.at("g2").grants[0];
  GatewayCustodyAckScope scope{};
  uint8_t key[32]{};
  // Before the signed snapshot is attached: a real epoch is refused.
  assert(!chain.activeGrant(g.tracker, g.epoch, scope, key));
  chain.setSigned(&boot.reader);
  assert(chain.activeGrant(g.tracker, g.epoch, scope, key));
  assert(memcmp(key, g.key, 32) == 0 && chain.signedIssued() == 1U);
  // A RAM grant does not shadow the signed one...
  uint8_t ram_key[32];
  memset(ram_key, 0x77, sizeof(ram_key));
  assert(chain.addGrant(g.tracker, g.epoch, 0U, 9U, ram_key) ==
         Sf5l0BenchGrantSource::AddResult::kAdded);
  assert(chain.activeGrant(g.tracker, g.epoch, scope, key) && memcmp(key, g.key, 32) == 0);
  // ...but serves a Tracker the snapshot does not hold.
  assert(chain.addGrant(0x1111222233334444ULL, 7U, 0U, 1U, ram_key) ==
         Sf5l0BenchGrantSource::AddResult::kAdded);
  assert(chain.activeGrant(0x1111222233334444ULL, 7U, scope, key) &&
         memcmp(key, ram_key, 32) == 0 && chain.signedIssued() == 2U);
  // The public test epoch still falls through to the test key.
  assert(chain.activeGrant(0x5555666677778888ULL, sf5l0_bench::kTestKeyEpoch, scope, key));
  // Denied snapshot (a write in progress): no signed grant any more; only
  // what the RAM table holds for that Tracker.
  boot.reader.denyForCurrentBoot();
  uint8_t k2[32]{};
  assert(chain.activeGrant(g.tracker, g.epoch, scope, k2) && memcmp(k2, ram_key, 32) == 0);
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  loadVectors(argv[1]);
  for (const char* n : {"g1", "g2", "g3_revoke", "g5", "wrong_gateway", "wrong_issuer"})
    assert(snaps.count(n) && snaps[n].lines.size() == 32U);
  testStagedView();
  testLines();
  testFirstSnapshotThenRotationThenRevocation(false);
  testFirstSnapshotThenRotationThenRevocation(true);
  testRefusals();
  testForeignRegionNotOverwritten();
  testFlashFaultAndTornPage();
  testUnreconciledMutationFaults();
  testGrantChain();
  printf("Step 3B signed Gateway grant snapshot: PASS\n");
  return 0;
}
