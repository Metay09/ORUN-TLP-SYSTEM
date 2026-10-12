// SF5E host run of the physical qualification probe itself
// (sf5e_observation_qual.cpp): every "boot" is a forked process with fresh
// globals; the product region and the protected regions above it live in a
// shared mapping at their real addresses, so flash survives reboots and
// software cuts exactly as on the device. Runs the same command sequence the
// remote script uses and checks every outcome line.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include <Arduino.h>
#include <nrf_sdm.h>

#include "storage_config.h"

using namespace orun_tlp::storage_config;

void setup();
void loop();

TestFicr ficr{kPageSize, 256, {0x71531AA3U, 0x0E8ADE7EU}};
TestUicr uicr{{0xF4000}};
TestWdt wdt{1U, {}};
TestRng rng{};
TestFicr* NRF_FICR = &ficr;
TestUicr* NRF_UICR = &uicr;
TestWdt* NRF_WDT = &wdt;
TestRng* NRF_RNG = &rng;

namespace orun_tlp::monotonic {
uint32_t nowMs() { return fake_millis; }
}

namespace {
constexpr uint32_t kMapStart = kProductRegionStart;
constexpr uint32_t kMapEnd = 0x0FE000U;  // through the bootloader code
int result_pipe = -1;

[[noreturn]] void finishBoot(int code) {
  const char* data = Serial.output.data();
  size_t left = Serial.output.size();
  while (left > 0) {
    const ssize_t written = write(result_pipe, data, left);
    if (written <= 0) break;
    data += written;
    left -= static_cast<size_t>(written);
  }
  close(result_pipe);
  _exit(code);
}
}  // namespace

void NVIC_SystemReset() { finishBoot(42); }

uint32_t sd_softdevice_is_enabled(uint8_t* out) {
  *out = 0;
  return NRF_SUCCESS;
}
uint32_t sd_flash_write(uint32_t* dst, const uint32_t* src, uint32_t words) {
  for (uint32_t i = 0; i < words; ++i) dst[i] &= src[i];
  return NRF_SUCCESS;
}
uint32_t sd_flash_page_erase(uint32_t page) {
  memset(reinterpret_cast<void*>(uintptr_t(page) * kPageSize), 0xFF, kPageSize);
  return NRF_SUCCESS;
}
uint32_t sd_evt_get(uint32_t*) { return NRF_ERROR_NOT_FOUND; }

namespace {

struct Boot {
  std::string output;
  bool reset = false;
};

unsigned boots = 0;

Boot boot(const std::string& commands) {
  ++boots;
  int fds[2];
  assert(pipe(fds) == 0);
  fflush(stdout);
  const pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(fds[0]);
    result_pipe = fds[1];
    srand(static_cast<unsigned>(getpid()));
    Serial.input = commands;
    setup();
    while (Serial.available()) loop();
    finishBoot(0);
  }
  close(fds[1]);
  Boot result;
  char buffer[4096];
  ssize_t got;
  while ((got = read(fds[0], buffer, sizeof(buffer))) > 0)
    result.output.append(buffer, static_cast<size_t>(got));
  close(fds[0]);
  int status = 0;
  assert(waitpid(pid, &status, 0) == pid);
  if (!WIFEXITED(status) ||
      (WEXITSTATUS(status) != 0 && WEXITSTATUS(status) != 42)) {
    fprintf(stderr, "boot %u crashed; output:\n%s\n", boots,
            result.output.c_str());
    abort();
  }
  result.reset = WEXITSTATUS(status) == 42;
  return result;
}

// The n-th (0-based) line starting with `prefix`, or the last one for n < 0.
std::string line(const std::string& out, const char* prefix, int n = -1) {
  std::string found;
  int index = 0;
  size_t start = 0;
  while (start < out.size()) {
    size_t end = out.find('\n', start);
    if (end == std::string::npos) end = out.size();
    const std::string current = out.substr(start, end - start);
    if (current.compare(0, strlen(prefix), prefix) == 0) {
      if (n < 0 || index == n) found = current;
      if (index == n) break;
      ++index;
    }
    start = end + 1;
  }
  return found;
}

std::string field(const std::string& text, const char* key) {
  const std::string needle = std::string(" ") + key + "=";
  const size_t at = text.find(needle);
  if (at == std::string::npos) return "";
  const size_t begin = at + needle.size();
  const size_t end = text.find(' ', begin);
  return text.substr(begin, end == std::string::npos ? std::string::npos
                                                     : end - begin);
}

long number(const std::string& text, const char* key) {
  const std::string value = field(text, key);
  assert(!value.empty());
  return strtol(value.c_str(), nullptr, 10);
}

void expectPass(const Boot& b, const char* prefix, int n = -1) {
  const std::string l = line(b.output, prefix, n);
  if (field(l, "result") != "PASS") {
    fprintf(stderr, "expected PASS for %s (boot %u):\n%s\n", prefix, boots,
            b.output.c_str());
    abort();
  }
}

void expectHealthyMount(const Boot& b) {
  const std::string store = line(b.output, "SF5E STORE", 0);
  if (field(store, "mounted") != "yes" || field(store, "faulted") != "no") {
    fprintf(stderr, "unhealthy mount (boot %u):\n%s\n", boots,
            b.output.c_str());
    abort();
  }
}

long records(const Boot& b, int n = -1) {
  return number(line(b.output, "SF5E VERIFY", n), "n");
}
long lastSequence(const Boot& b, int n = -1) {
  return number(line(b.output, "SF5E VERIFY", n), "last");
}
long releasedRecords(const Boot& b, int n = -1) {
  return number(line(b.output, "SF5E VERIFY", n), "rel");
}

// One cut around one command, then the reboot check: the store mounts
// healthy, every committed record is intact and contiguous, maintenance can
// finish and appends continue. Counts can drop when maintenance reclaims the
// oldest page, so progress is judged by the newest committed sequence.
// Returns false when the command never reached the armed call (the sequence
// of mutations is shorter than `call`).
bool cutAndRecover(const char* setup_commands, const char* command,
                   unsigned call, const char* mode, long* committed_delta) {
  const Boot before = boot(std::string("SF5E VERIFY\n") + setup_commands);
  expectPass(before, "SF5E VERIFY", 0);
  char cut[64];
  snprintf(cut, sizeof(cut), "SF5E CUT %u %s\n", call, mode);
  if (getenv("SF5E_TRACE")) printf("    cut %s call=%u %s\n", command, call, mode);
  const Boot cut_boot =
      boot(std::string("SF5E VERIFY\n") + cut + command + "\n");
  expectPass(cut_boot, "SF5E VERIFY");
  const long last1 = lastSequence(cut_boot);
  if (!cut_boot.reset) {
    assert(cut_boot.output.find("SF5E CUT not_reached") != std::string::npos);
    return false;
  }
  assert(cut_boot.output.find("SF5E CUT fired") != std::string::npos);
  const Boot after =
      boot("SF5E VERIFY\nSF5E MAINT\nSF5E APPEND 1\nSF5E VERIFY\n");
  expectHealthyMount(after);
  expectPass(after, "SF5E VERIFY", 0);
  expectPass(after, "SF5E MAINT");
  expectPass(after, "SF5E APPEND");
  expectPass(after, "SF5E VERIFY", 1);
  const long recovered = lastSequence(after, 0);
  assert(recovered >= last1 && recovered <= last1 + 1);
  assert(lastSequence(after, 1) == recovered + 1);
  if (committed_delta) *committed_delta = recovered - last1;
  return true;
}

void maintenanceMatrix(const char* label) {
  const char* modes[] = {"BEFORE", "TORN", "AFTER"};
  for (const char* mode : modes) {
    unsigned call = 1;
    for (; call <= 24U; ++call) {
      if (!cutAndRecover("SF5E FILLPAGE\n", "SF5E MAINT", call, mode,
                         nullptr))
        break;
    }
    assert(call > 1U && call <= 24U);
    printf("  %s maintenance %s: %u cut points recovered\n", label, mode,
           call - 1U);
  }
}

}  // namespace

int main() {
  void* mapped =
      mmap(reinterpret_cast<void*>(kMapStart), kMapEnd - kMapStart,
           PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  assert(mapped == reinterpret_cast<void*>(kMapStart));
  // Leftovers in the product region (old images, test markers) and live
  // data in every protected region.
  auto* bytes = static_cast<uint8_t*>(mapped);
  srand(12345);
  for (uint32_t i = 0; i < kMapEnd - kMapStart; ++i)
    bytes[i] = static_cast<uint8_t>(rand());
  for (uint32_t page = 27; page < kProductRegionPages; ++page)
    memset(bytes + page * kPageSize + 16U, 0xFF, kPageSize - 16U);

  // 1. Leftovers are refused until PREPARE; append, verify, reboot.
  const Boot first = boot(
      "SF5E VERIFY\nSF5E PREPARE CONFIRM\nSF5E APPEND 10\nSF5E VERIFY\n"
      "SF5E REBOOT\n");
  assert(field(line(first.output, "SF5E REGION", 0), "decision") ==
         "NEEDS_PREPARATION");
  assert(first.output.find("SF5E rejected: store not mounted") !=
         std::string::npos);
  expectPass(first, "SF5E PREPARE result");
  expectPass(first, "SF5E APPEND");
  expectPass(first, "SF5E VERIFY");
  assert(records(first) == 10);
  assert(first.reset);
  const std::string protect0 = line(first.output, "SF5E PROTECT", 0);
  assert(!protect0.empty());

  const Boot second = boot("SF5E VERIFY\n");
  expectHealthyMount(second);
  expectPass(second, "SF5E VERIFY");
  assert(records(second) == 10);
  assert(field(line(second.output, "SF5E REGION"), "decision") == "MOUNT");

  // 2. Append cut points (body write = call 1, commit word = call 2).
  struct Cut { unsigned call; const char* mode; long delta; };
  const Cut append_cuts[] = {{1, "BEFORE", 0}, {1, "TORN", 0}, {1, "AFTER", 0},
                             {2, "BEFORE", 0}, {2, "AFTER", 1}};
  for (const Cut& c : append_cuts) {
    long delta = -1;
    assert(cutAndRecover("", "SF5E APPEND 1", c.call, c.mode, &delta));
    assert(delta == c.delta);
  }
  printf("  append: 5 cut points recovered\n");

  // 3. Release marker cut points.
  for (const char* mode : {"BEFORE", "AFTER"}) {
    const Boot before = boot("SF5E VERIFY\n");
    const long released0 = releasedRecords(before);
    char cut[48];
    snprintf(cut, sizeof(cut), "SF5E CUT 1 %s\n", mode);
    const Boot cut_boot = boot(std::string(cut) + "SF5E RELEASE 1\n");
    assert(cut_boot.reset);
    const Boot after = boot("SF5E VERIFY\nSF5E RELEASE 1\nSF5E VERIFY\n");
    expectHealthyMount(after);
    expectPass(after, "SF5E VERIFY", 0);
    expectPass(after, "SF5E RELEASE");
    const long expected = released0 + (strcmp(mode, "AFTER") == 0 ? 1 : 0);
    assert(releasedRecords(after, 0) == expected);
    assert(releasedRecords(after, 1) == expected + 1);
  }
  printf("  release: 2 cut points recovered\n");

  // 4. Page maintenance (prepare/activate) cut points before the store wraps.
  maintenanceMatrix("before wrap");

  // 5. Fill until the store reclaims its oldest page, then cut reclaim.
  long reclaimed = 0;
  for (unsigned guard = 0; guard < 40U && reclaimed == 0; ++guard) {
    const Boot fill = boot("SF5E FILLPAGE\nSF5E MAINT\n");
    expectPass(fill, "SF5E FILLPAGE");
    expectPass(fill, "SF5E MAINT");
    reclaimed = number(line(fill.output, "SF5E DIAG"), "reclaimed");
  }
  assert(reclaimed > 0);
  maintenanceMatrix("after wrap");

  // 6. Nothing outside the product region changed; PREPARE leaves it erased.
  const Boot last = boot(
      "SF5E VERIFY\nSF5E STATUS\nSF5E PREPARE CONFIRM\nSF5E VERIFY\n");
  expectPass(last, "SF5E VERIFY", 0);
  assert(line(last.output, "SF5E PROTECT") == protect0);
  expectPass(last, "SF5E PREPARE result");
  assert(field(line(last.output, "SF5E REGION"), "erased") == "32");
  assert(records(last) == 0);

  assert(munmap(mapped, kMapEnd - kMapStart) == 0);
  printf("SF5E qualification probe host run (%u boots): PASS\n", boots);
}
