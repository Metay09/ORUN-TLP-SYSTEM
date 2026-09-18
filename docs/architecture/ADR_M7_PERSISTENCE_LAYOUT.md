# ADR: M7 Persistence Layout — Partition Plan + SoftDevice Flash Concurrency Contract

Status: **DECIDED (design/ADR only) — no runtime, protocol, BLE, DFU, ACK/security,
or history-format implementation in this change.**

Milestone: **M7P1**, a prerequisite decision gate for M7 BLE/DFU and for any
future durable configuration, security material, or authenticated
store-forward work. It resolves the **E-stage** ("storage/concurrency
decisions required before M7 BLE runtime") item left `UNRESOLVED` by
`docs/architecture/ORUN_STORAGE_FLASH_OWNERSHIP.md` §17. It does **not**
resolve stage F (physical bootloader/DFU facts) or stage G (authenticated
ACK/store-forward durable-state invariants) — those remain their own
prerequisites, tracked in §17.

Baseline: `main@c97682677e357929258db08bae96594839d919a5` (storage/flash
ownership audit, PR #10, merged).
Branch: `design/m7p1-persistence-layout`.

## 1. Context

`ORUN_STORAGE_FLASH_OWNERSHIP.md` established, from source, that:

- The application is linked into `0x026000..0x0ED000` (815,104 bytes).
- The history journal exclusively owns `0x0ED000..0x0F4000` (7×4,096-byte
  pages) and must never be shared with `InternalFS`/LittleFS.
- A stock `Bluefruit.begin()` → `bond_init()` → `InternalFS.begin()` call
  would erase and reformat that exact history region on its first failed
  LittleFS mount — a destructive conflict, not a passive one.
- The current `NrfHistoryFlash` backend is synchronous-only and fails closed
  the instant SoftDevice is enabled, because `sd_flash_write`/
  `sd_flash_page_erase` complete asynchronously (via
  `NRF_EVT_FLASH_OPERATION_SUCCESS`/`ERROR`) once SoftDevice is running.
- No owner exists today for BLE bonds, durable configuration, security
  material, or anti-replay counters. All were `BLOCKED`/`UNALLOCATED`.

This ADR answers: *where do these classes live, in what backend, under what
concurrency/arbitration rule, and what exactly changes at the address level
— without implementing any of it.*

## 2. Current verified map (carried forward, unchanged)

| Range | Owner | Status in this ADR |
| --- | --- | --- |
| `0x000000..0x026000` | MBR + SoftDevice reserved | **Unchanged. Not touched.** |
| `0x026000..0x0ED000` | Application (815,104 B) | **Effective linkable ceiling reduced by this ADR's recommendation — see §6/§7.** The underlying linker `MEMORY` region itself is not changed by this ADR (see §7's enforcement-mechanism discussion). |
| `0x0ED000..0x0F4000` | ORUN history journal, exclusive | **Unchanged. Not touched, not shrunk, not shared.** |
| `0x0F4000..0x0FF000` | Bootloader-owned boundary region | **Unchanged. Not touched.** |
| `0x0FF000..0x100000` | Bootloader settings page | **Unchanged. Not touched.** |

Every one of the "invariants to preserve" listed in the task brief for this
milestone is satisfied by construction: the new reservation in §7 is carved
from currently-unused *application* headroom, strictly below `0x0ED000` and
strictly above `0x026000`, never touching history or bootloader ranges.

## 3. Requirements (quantified, not assumed)

### 3A. BLE bonds

- `bond_keys_t` (`Bluefruit52Lib/src/utility/bonding.h`) is documented in its
  own source comment as **80 bytes** per bonded peer (`own_enc` +
  `peer_enc` + `peer_id`, the GAP encryption/identity key set).
- Storage backend is Adafruit's `Adafruit_LittleFS` over `InternalFS`, with
  `LFS_BLOCK_SIZE = 128` bytes (`InternalFileSystem.cpp`). One file per
  bonded peer under `/adafruit/bond_prph/<addr>` or `/adafruit/bond_cntr/<addr>`
  (`bonding.h`: `BOND_DIR_PRPH`/`BOND_DIR_CNTR`/`BOND_FNAME_*`).
- Engineering estimate (not a measured LittleFS trace): a small root
  superblock plus two bond-directory metadata regions cost on the order of a
  few 128-byte blocks each (LittleFS keeps a small redundant metadata-pair
  log per directory); each ~80-byte bond file rounds up to at least one
  128-byte block, plausibly two with wear-leveling/metadata redundancy. This
  is stated as an **estimate with explicit uncertainty**, not a proven exact
  layout — the actual bundled LittleFS/Adafruit_LittleFS version was not
  traced block-by-block for this ADR.
- ORUN's expected peer count is small: current and near-term ORUN nodes are
  not designed as multi-tenant BLE hubs — a TRACKER/RELAY/BASE pairs with at
  most a handful of companion phones/operator tools over the product's life,
  not tens of simultaneous bonds. Even a conservative estimate (fixed
  overhead plus ~256 bytes/bond) comfortably fits **10+ bonded peers within a
  single 4,096-byte page**, and easily within **2 pages (8,192 bytes)** with
  a wide safety margin, without inventing a specific peer-count requirement
  the product has not stated.
- Stock Adafruit's own default reservation for this exact reason (bonds plus
  whatever else a stock sketch stores) is the entire 7-page (28,672-byte)
  `InternalFS` default — the same size as ORUN's history journal, because
  ORUN's journal simply took over that pre-existing reservation. This is
  evidence that even a generously-sized bond+filesystem allocation for this
  MCU class is an order of magnitude smaller than "most of application
  flash"; it does not by itself justify any particular ORUN page count.

### 3B. Durable device/configuration

No config schema exists today (`ORUN_CURRENT_ARCHITECTURE_RULES.md` §6: "B4/M6
current configuration remains runtime-only"). This ADR does not invent one.
What is architecturally required, independent of the exact fields:

- **Atomic old/new update**: nRF52 flash erases only at page granularity, so
  an in-place field update cannot be atomic; a versioned A/B (two-page,
  ping-pong) scheme is the minimum viable atomicity primitive — write the
  full new candidate to the inactive page with a generation number and a
  final commit word (the same body-then-commit-word-last invariant already
  proven in `journal_format`/`HistoryStore`), then treat the higher
  committed generation as active. **Minimum: 2 pages.**
- Even a config schema significantly richer than anything currently
  envisioned (profile/service selection, tracking interval, location-source
  policy, RF parameters, geofence area references, user-facing settings) is
  well under one 4,096-byte page in raw size; 2 pages leaves a full spare
  page's worth of headroom for schema growth and versioned migration without
  redesigning the partition.

### 3C. Security material

Explicitly **out of scope**: choice of algorithm, key derivation, or
provisioning protocol (`AGENTS.md`: "Do not invent cryptography"). In scope:
storage/lifecycle requirements only.

- Durable, versioned, anti-rollback: same 2-page A/B atomicity primitive as
  config, but **must be a physically separate partition from config**, so
  that an ordinary config reset (§11) cannot touch keys, and so a future
  security audit's blast radius is exactly this partition, not "config plus
  keys." **Minimum: 2 pages.**
- Key material itself (symmetric keys, key generation/version metadata) is
  small (tens of bytes per key in any mature scheme); 2 pages is generous
  headroom for a small number of key generations plus rotation history, not
  a byte-tight allocation.

### 3D. Anti-replay durable counters/state

Must be a **distinct counter namespace from the history journal's
`SequenceSource`/ticket reservation** (`storage_config::kSequenceBlockSize`),
per `ORUN_SYSTEM_ARCHITECTURE_V1.md` §16: "separate counters from legacy
wrapping uint32 sequence and ordinary history reclamation." It shares
security material's durability/anti-rollback/isolation requirements (§3C)
closely enough that this ADR places it in the **same physical partition as
security material** as a distinct record type — not merged into one blob,
and never sharing a page/record with config or history. This avoids
fragmenting into a fourth tiny partition for a handful of monotonic counters
that need exactly the same protection class as the keys they guard.

### 3E. BLE/DFU metadata

The bootloader's own settings page (`0xFF000`) and any bootloader-internal
book-keeping are **not reallocated or duplicated here** — this ADR does not
create an application-owned shadow of bootloader state. No application-owned
DFU metadata partition is allocated, because no concrete requirement for one
exists yet (the product does not yet implement DFU-triggered application
logic beyond "accept the bootloader's own image update"). If a genuine
requirement appears later (e.g., an app-level "last successful update"
marker), it is small and can be added to the config partition's schema at
that time — not pre-allocated speculatively now.

## 4. Options considered

### Option A — Single relocated InternalFS partition for bonds *and* config; security separate raw partition

Move `InternalFS`'s `LFS_FLASH_ADDR`/`LFS_FLASH_TOTAL_SIZE` (via a core patch,
§8) to a new region and also store ORUN's config there as LittleFS files;
security material gets its own raw versioned partition.

- Pages/bytes: ~3–4 pages for the shared LittleFS partition (bonds + config)
  + 2 for security = 5–6 pages.
- Complexity: **higher** — config would need its own LittleFS file
  read/write/format-migration logic in addition to the bond code already
  provided by Bluefruit, plus a defined behavior for what happens to config
  if `InternalFS.begin()`'s failed-mount auto-erase path is ever triggered
  (it would take config down with it, not just bonds).
- Power-cut behavior: LittleFS provides its own wear-leveling/power-safety
  contract, but ORUN would then depend on *two different* power-cut models
  (LittleFS's for bonds+config, the existing journal model for history) —
  more to reason about, not less.
- Factory-reset semantics: a single LittleFS partition mixing bonds and
  config makes "unpair BLE but keep config" or "reset config but keep bonds"
  require selective directory operations inside one filesystem instead of
  simply erasing/not-erasing a whole separate partition.
- SoftDevice compatibility: same async constraint as bonds alone (§9).
- Security isolation: fine (security is already separate in this option).
- Dependency/migration risk: **higher** — couples ORUN's own config format
  to Adafruit's LittleFS/Adafruit_LittleFS library behavior and version,
  which this repo does not otherwise depend on for anything except bonds.

### Option B — Fully raw/versioned fixed-page stores for bonds, config, and security; no LittleFS anywhere

Abandon Adafruit's stock bond storage entirely; write a custom raw bond
record store (mirroring `journal_format`'s pattern) and patch
`bonding.cpp`'s many `InternalFS`/`Adafruit_LittleFS_Namespace::File` call
sites to use it instead.

- Pages/bytes: potentially the smallest total (bond records are tiny, no
  filesystem metadata overhead) — plausibly 1 page for bonds + 2 for config
  + 2 for security = 5 pages.
- Complexity: **highest** — `bonding.cpp` calls `InternalFS`/LittleFS file
  APIs at more than a dozen call sites (`exists`, `mkdir`, `File` construction
  and read/write, `rmdir_r`, `remove`), not through one swappable seam. A
  full custom backend means patching (and re-auditing on every core upgrade)
  all of those call sites, not the two `#define` constants Option C needs.
- Power-cut behavior: fully ORUN-controlled and consistent with the existing
  journal model (a real advantage), but only after writing and host-testing
  an entirely new bond-record format from scratch.
- Factory-reset semantics: clean (dedicated pages per class).
- SoftDevice compatibility: same async constraint (§9), same either way.
- Security isolation: fine.
- Dependency/migration risk: **highest patch surface** against a vendored
  library that Bluefruit itself may change between Adafruit core versions;
  every core upgrade requires re-verifying many call sites instead of two
  constants.

### Option C — Relocate `InternalFS` for bonds only (core patch); dedicated raw versioned partitions for config and for security+anti-replay (chosen)

Reuse the exact two-`#define` relocation for bonds (small, low-risk,
single-purpose core patch, reusing the repository's own established
patch-script pattern — see §8). Give config and security+anti-replay their
own dedicated raw, versioned, A/B fixed-page partitions, architecturally
mirroring `HistoryStore`'s existing page/generation/commit-word pattern
(a sibling backend/format, **not** the same physical pages or instance as
history) instead of forcing them into a filesystem abstraction they do not
need.

- Pages/bytes: 2 (bonds, LittleFS) + 2 (config, raw A/B) + 2 (security +
  anti-replay, raw A/B) = **6 pages, 24,576 bytes**.
- Complexity: **lowest total for the value delivered** — the bond patch
  touches exactly two constants in one already-audited file class (matching
  existing `patch_wire.py`/`patch_radio.py` precedent, §8); config/security
  reuse an already-proven, already-host-tested architectural pattern
  (page/generation/CRC/commit-word) instead of a new filesystem dependency.
- Power-cut behavior: config and security inherit the *already-verified*
  commit-word-last invariant family (new instances, new dedicated pages, not
  shared code/state with history, but the same reasoned model) — the audit
  document's power-cut confidence directly informs both new partitions'
  design contract without duplicating history's actual code/state.
- Factory-reset semantics: three independently erasable partitions map
  cleanly onto three independent reset semantics (§11) with no shared
  filesystem to reason about.
- SoftDevice compatibility: bonds still need the async contract (§9)
  because `InternalFS`/`flash_nrf5x` ultimately also call `sd_flash_*`
  once SoftDevice is enabled; config/security need it too for any BLE-time
  write. This option does not remove that requirement, but it does not add
  to it either, versus A or B.
- Security isolation: **best of the three** — security material is never in
  the same filesystem or even the same backend family as bonds, and a
  corrupted/reformatted bond `InternalFS` partition cannot touch config or
  keys.
- Dependency/migration risk: **lowest** — smallest vendor patch surface (two
  constants, one file), and config/security reuse in-repo, already-tested
  code patterns instead of a new library dependency surface.
- Future M7 practicality: bonds keep working exactly as Bluefruit expects
  (same API, same directory names, just a relocated backing region), so
  BLE/pairing code written against stock Bluefruit APIs needs no behavior
  changes — only the *build-time* relocation patch.

**Chosen: Option C.** It has the smallest patch surface, the best security
isolation, and reuses already-proven, already-host-tested architecture
(the journal's page/generation/commit-word pattern) instead of adding a new
dependency (a from-scratch bond format) or coupling unrelated data classes
into one filesystem (Option A).

## 5. Chosen layout

Six new pages, carved from currently-unused application headroom,
**immediately below** the unchanged history region, addresses increasing
toward history:

```text
0x026000 .. 0x0E7000   Application (reduced ceiling; see §6/§7)
0x0E7000 .. 0x0E9000   Security material + anti-replay counters (2 pages, raw A/B)
0x0E9000 .. 0x0EB000   Durable device/configuration (2 pages, raw A/B)
0x0EB000 .. 0x0ED000   BLE bonds (2 pages, relocated InternalFS/LittleFS)
0x0ED000 .. 0x0F4000   History journal (UNCHANGED, 7 pages, exclusive)
0x0F4000 .. 0x100000   Bootloader + settings (UNCHANGED)
```

Ordering rationale: no functional requirement forces this exact order among
the three new partitions (they are independently addressed, fixed-size, and
never accessed as one contiguous range) — the order shown groups the two
raw/versioned partitions (security, config) adjacently by backend family,
with the filesystem-backed bonds partition placed last, directly adjacent to
history, changing nothing about history's own address.

## 6. Application headroom impact

| | Bytes | % of respective ceiling |
| --- | ---: | ---: |
| Current application ceiling | 815,104 | 100% |
| Current firmware size (`main@c976826`) | 147,144 | 18.1% of 815,104 |
| Current headroom | 667,960 | 81.9% of 815,104 |
| New reservation (6 pages) | 24,576 | 3.0% of 815,104 |
| **New application ceiling** | **790,528** | 100% |
| Current firmware size against new ceiling | 147,144 | **18.6%** of 790,528 |
| **New headroom** | **643,384** | **81.4%** of 790,528 |

The reservation is justified by the quantified requirements in §3/§4, not by
"plenty of room exists" — the headroom figures above are a sanity check that
the specific, derived 6-page requirement is affordable, not the reason for
choosing it.

**Enforcement is the open question this ADR resolves explicitly, not
implicitly**: the vendored `nrf52840_s140_v6.ld` linker script's `MEMORY`
region still physically permits linking all the way to `0x0ED000` unless
that file itself is patched. Two enforcement mechanisms were considered:

- **Soft ceiling (recommended for the first implementation slice, M7P2)**: a
  new post-link build-guard check (same idiom as
  `check_storage_layout.py`'s existing `check_exclusive_owner`, using
  `arm-none-eabi-size`/`nm` against the built ELF) that fails the build if
  the linked application's highest used address exceeds `0x0E7000`. This
  requires **no vendor core patch** and no change to the existing hard
  requirement that the linker script text match `nrf52840_s140_v6.ld`
  exactly. Trade-off: it is a build-time policy, not a hardware/linker-level
  reservation — a build run with the guard script bypassed or removed could
  still link into that space, unlike a true `MEMORY` region change.
- **Hard linker patch (future hardening option, not required for M7P1/M7P2)**:
  patch the linker script's `LENGTH` for the `FLASH` region down to
  `0x0E7000 - 0x026000`, using the exact same pinned-blob-hash patch-script
  pattern already established for `patch_wire.py`/`patch_radio.py` (§8's
  bond relocation uses the identical idiom). This gives a true linker-level
  guarantee at the cost of introducing a third vendor core patch and
  requiring `check_storage_layout.py`'s linker-content check to validate the
  *patched* text instead of the stock text.

This ADR recommends starting with the **soft ceiling guard** in M7P2 (lower
risk, no new vendor patch, consistent with "smallest viable architecture"),
with the hard linker patch available as a later hardening step if the team
wants a stronger guarantee once the partitions are actually implemented and
in active use.

## 7. Exact partition plan

| Field | Security + anti-replay | Durable config | BLE bonds |
| --- | --- | --- | --- |
| Address start | `0x0E7000` | `0x0E9000` | `0x0EB000` |
| Address end (exclusive) | `0x0E9000` | `0x0EB000` | `0x0ED000` |
| Page count | 2 | 2 | 2 |
| Owner (future component) | new `SecurityStore` (name indicative, decided at implementation) | new `ConfigStore` (name indicative) | relocated `InternalFS` (stock Adafruit type, patched address) |
| Format/backend | Raw, versioned A/B, journal-family commit-word-last pattern (new sibling of `journal_format`, not shared code/state with history) | Same raw A/B pattern as security | LittleFS via `Adafruit_LittleFS`/`InternalFileSystem` (unchanged library code, patched `LFS_FLASH_ADDR`/`LFS_FLASH_TOTAL_SIZE`) |
| Erase owner | New `SecurityStore` only, through the shared `FlashMutationGate` (§9) | New `ConfigStore` only, through the same gate | `Adafruit_LittleFS`/`flash_nrf5x`, through the same gate |
| Reset semantics | See §11 — never erased by ordinary config reset or BLE unpair | See §11 — reset-to-defaults only, never touches security or bonds | See §11 — BLE unpair/re-provision only, never touches config or security |
| Security class | Highest (keys, anti-replay) | Low–moderate | Moderate (peer key material) |
| SoftDevice interaction | Requires the async contract (§9) before any write once BLE is enabled | Same | Same (bonds cannot be written meaningfully without SoftDevice enabled in the first place) |

History's address (`0x0ED000..0x0F4000`) does not change. No field in this
table is a runtime implementation; it is the target for the implementation
slices in §16.

## 8. Bond backend decision

Verified directly from the installed Adafruit nRF52 1.7.0 core:

- `AdafruitBluefruit::begin()` (`bluefruit.cpp`) calls `bond_init()`
  unconditionally.
- `bond_init()` (`utility/bonding.cpp`) calls `InternalFS.begin()`
  unconditionally, then creates `BOND_DIR_PRPH`/`BOND_DIR_CNTR`.
- `InternalFS` is a fixed global singleton
  (`extern InternalFileSystem InternalFS;`, `InternalFileSystem.h`) —
  **there is no constructor parameter, factory, or public API to point
  Bluefruit's bonding subsystem at a different filesystem instance or a
  different flash region.** The address is compiled in via `#define
  LFS_FLASH_ADDR`/`LFS_FLASH_TOTAL_SIZE` inside `InternalFileSystem.cpp`
  (`#ifdef NRF52840_XXAA ... 0xED000 ...`).
- `bonding.cpp` calls `InternalFS`/`Adafruit_LittleFS_Namespace::File`
  directly at more than a dozen call sites (`exists`, `mkdir`, file
  read/write/remove, `rmdir_r`), **not through one swappable seam** — there
  is no documented or apparent way to make the bond manager "accept another
  backend" without patching most of `bonding.cpp`.

**Answering the required questions directly:**

- *Which path does `Bluefruit.begin()` use?* `bond_init()` →
  `InternalFS.begin()`, unconditionally.
- *Is `InternalFS` mandatory?* Yes, as shipped; Bluefruit's bonding code has
  no alternate backend hook.
- *Is the base/size compile-time overridable?* Only by editing the vendored
  `InternalFileSystem.cpp` source (a core patch), not via any public
  `-D` flag, config header, or constructor parameter.
- *Can a separate custom `InternalFS` region be supported?* Yes — relocating
  the two `#define` values is sufficient, because `InternalFS`'s own
  internal driver (`flash_nrf5x`) is parameterized entirely by those two
  macros, not by any other hardcoded address.
- *Does this require a library patch?* Yes — exactly the same style of
  targeted, pinned, auto-restoring vendor patch already used twice in this
  repository (`firmware/scripts/patch_wire.py` for R4,
  `firmware/scripts/patch_radio.py` for R2.1). A new
  `patch_internalfs.py` would follow the identical idiom: hash-pin the
  exact upstream `InternalFileSystem.cpp` blob (`git hash-object` on the
  currently-installed file gives `923aede3cd736ac4a7caf75fed982aeb2c726136`,
  recorded here as the audited baseline for that future patch's own
  `ORIGINAL_GIT_BLOB_SHA` pin — not yet written), replace exactly the two
  `#define` literals for the `NRF52840_XXAA` branch, verify replacement
  count is exactly one each (`replace_once`-style), and restore the
  original file at process exit via `atexit`, exactly like `patch_wire.py`.
- *Does the bond manager accept another backend?* No, not without a much
  larger patch to `bonding.cpp` itself (Option B in §4), which this ADR
  rejects in favor of the smaller, single-purpose relocation patch.

**Decision: relocate `InternalFS` via a core patch** (Option C, §4). The
bond directory structure, file naming, and all of Bluefruit's own pairing
logic remain completely unmodified — only the two address/size constants
move, to `0x0EB000`/`0x2000` (2 pages), away from `0x0ED000`. This never
overlaps history, by construction. Writing this patch script is an
implementation task for a later slice (§16), not done in this ADR.

## 9. Async SoftDevice flash contract

Verified constraint (from the installed S140 6.1.1 `nrf_soc.h`): once
SoftDevice is enabled, `sd_flash_write`/`sd_flash_page_erase` only *start* an
operation; completion arrives later as `NRF_EVT_FLASH_OPERATION_SUCCESS` or
`NRF_EVT_FLASH_OPERATION_ERROR` through the SoftDevice event queue, and the
source buffer passed to `sd_flash_write` "should not be modified" until that
event arrives. Today's `NrfHistoryFlash` assumes immediate synchronous
completion and cannot be reused as-is (`ORUN_STORAGE_FLASH_OWNERSHIP.md` §8).

This ADR defines the **contract** a future asynchronous backend must satisfy
— not its implementation:

- **Single flash mutation owner**: a new small seam, named indicatively
  `FlashMutationGate` (mirroring the existing `radio_driver_gate.h`
  acquire/release/`Guard` idiom exactly — same repository style already used
  to serialize the one physical radio), is the *only* caller of
  `sd_flash_write`/`sd_flash_page_erase` once SoftDevice is enabled. No
  store (history, config, security, bonds/`InternalFS`) calls those Nordic
  APIs directly under SoftDevice-enabled operation.
- **Request origin**: `HistoryStore`, the future `ConfigStore`/`SecurityStore`,
  and `InternalFS`'s own driver (`flash_nrf5x`, reached indirectly through
  Bluefruit) each submit one bounded request at a time to the gate; the gate
  holds at most one physical operation in flight, mirroring the existing
  "one active radio TX" ownership model.
- **Event routing**: whatever already pumps the SoftDevice event queue for
  BLE (Bluefruit's own scheduler/`sd_evt_get` loop, invoked from the
  existing cooperative `main.cpp` loop, not a new RTOS task) must also
  forward `NRF_EVT_FLASH_OPERATION_SUCCESS`/`ERROR` into
  `FlashMutationGate`'s completion handler. This is one additional case in
  an already-necessary event dispatch, not a new polling mechanism.
- **Source buffer ownership**: each store retains its *own* fixed staging
  buffer as a member (exactly `HistoryStore::blob_[kPageHeaderSize]`'s
  existing pattern), sized for its own maximum blob, and does not release or
  reuse it until the gate reports completion. No shared or heap-allocated
  transient buffer is introduced.
- **"Pending" state in `HistoryStore`**: `writeBlob()` would submit to the
  gate and return "pending" instead of a synchronous boolean; a new explicit
  state (e.g. widening `Phase`/`Job` with a pending flag) gates `poll()`
  from advancing until the gate's completion callback fires. This is a
  contract requirement for whichever implementation slice touches
  `HistoryStore` for SoftDevice-enabled operation (§16); no such change is
  made in this ADR.
- **`PositionFlow` may not bypass storage completion.** This is a hard
  requirement, not a design choice re-opened here: store-before-send
  (`AGENTS.md`; `docs/milestones/M4.md`) means a live POSITION TX still
  waits for the (now possibly-asynchronous) append/readback to report
  success, exactly as it does today for the synchronous path. An async
  completion event is just another trigger for `PositionFlow`/`poll()` to
  advance state — never a reason to transmit before storage confirms.
- **Reentrancy between the BLE event pump and the flash state machine**: is
  prevented the same way the existing radio driver-gate prevents overlapping
  radio operations — `FlashMutationGate` exposes a single in-flight boolean
  gate, checked before starting any new physical `sd_flash_*` call, so an
  event arriving while another submission is being prepared cannot start a
  second concurrent hardware operation. No new concurrency primitive beyond
  this existing idiom is introduced.
- **Timeout/error handling**: `NRF_EVT_FLASH_OPERATION_ERROR` completes the
  pending request as a failure, propagated to the originating store exactly
  like today's synchronous `false` return. A bounded timeout fallback for an
  event that never arrives (SoftDevice fault) is a required design element
  for the implementation slice that builds this gate (§16) — consistent with
  this repo's existing bounded-recovery philosophy
  (`kMaxI2cRecoveriesPerAcquisition`, `WatchdogManager`) — but the exact
  timeout value is an implementation detail, not decided here.
- **SoftDevice enable/disable transitions**: the backend mode (synchronous
  vs. asynchronous) is fixed for the lifetime of one boot, selected once
  when BLE is enabled, extending the *already-existing* invariant that
  "M0-M5 has one main-loop storage owner and never enables SoftDevice;
  concurrent enable/disable is not supported" (`docs/storage/M4_FLASH_JOURNAL.md`).
  Live hot-swapping between modes within one boot is explicitly not
  required or designed.
- **Concurrent bond/config/history/security requests**: arbitrated entirely
  by `FlashMutationGate`'s bounded queue and the priority order in §10 — no
  store ever reasons about another store's flash access directly.

## 10. Concurrency / priority

A small **bounded** admission queue (fixed-capacity array, no heap — the
same style as `NetworkService`'s existing 4-entry relay queue), holding at
most one entry per request class plus the one in-flight operation, with a
fixed priority order for admission when more than one class has a pending
request:

1. **Critical security durability** (key rotation/provisioning commit,
   anti-replay counter advance) — highest priority; these are rare but must
   never be starved, since a stalled anti-replay commit blocks whatever
   protected operation depends on it.
2. **Live store-before-send history append** (position/critical event) —
   must not be starved by lower classes, extending the existing principle
   "live data must not wait behind a large historical backlog" (`AGENTS.md`)
   from RF airtime into flash admission.
3. **Config writes** — user/BLE-initiated, infrequent.
4. **Bond writes** — BLE pairing-time only; delaying a bond commit by one
   arbitration slot delays completing a pairing ceremony slightly, not a
   safety-relevant outcome.

This is a small explicit priority list, not a generic weighted-fairness
scheduler — consistent with "bounded queue / single operation ownership" and
against building speculative scheduling infrastructure this product does not
yet need.

**Power-cut/reset pending-operation semantics**: a power cut mid-operation
leaves the physical flash in whatever state the hardware left it in (no
different from today's synchronous model — NOR programming is still NOR
programming). The only *new* risk from asynchrony is that in-RAM queue state
(which request was pending, which store submitted it) is lost on reset. This
is handled by requiring, as an explicit contract element, that **every
store's boot-time recovery must reconstruct valid state purely from
committed flash content**, exactly like `HistoryStore::recover()` already
does — never from a surviving RAM-resident queue or "was pending" flag. No
store may assume a request survived reset merely because it was submitted
before the cut.

## 11. Reset/factory-reset semantics

| Persistent class | Ordinary reboot | Config reset | BLE unpair | Factory reset | Secure decommission |
| --- | --- | --- | --- | --- | --- |
| History journal (`0x0ED000..0x0F4000`) | Preserved (existing behavior, unchanged) | **Preserved** — config reset must never touch history | Preserved | Explicit, separate action required (not implemented; no such command exists today per `ORUN_STORAGE_FLASH_OWNERSHIP.md` §10) | Explicit secure-erase action, separate from ordinary factory reset |
| Durable config (`0x0E9000..0x0EB000`) | Preserved | **Reset to defaults** (this is what "config reset" means) | Preserved — unpairing a phone must not reset tracking/service configuration | Reset to defaults | Reset to defaults |
| Security material + anti-replay (`0x0E7000..0x0E9000`) | Preserved | **Preserved** — an ordinary config reset must never clear keys or roll back anti-replay counters | Preserved — unpairing one peer must not touch provisioned network/device keys | Requires an **explicit, separate secure-erase policy** (not designed here — a future security milestone's decision), never an implicit side effect of factory-resetting config | Explicit secure-erase target |
| BLE bonds (`0x0EB000..0x0ED000`) | Preserved | Preserved — resetting config must not force every phone to re-pair | **Cleared** (this is what "BLE unpair" means — `bond_clear_prph`/`bond_clear_all` already exist in stock Bluefruit) | Cleared | Cleared |
| Bootloader/settings (`0x0F4000..0x100000`) | N/A — not application-owned | N/A | N/A | N/A — **not in scope of any application-level reset**; this ADR does not define bootloader reset behavior | N/A |

The three explicit invariants called out in the task brief are satisfied by
this table: history is never touched by BLE unpair; keys are never touched
by ordinary config reset; factory reset of security requires its own
explicit policy rather than being folded into general factory reset.

## 12. Power-cut invariants

Config and security/anti-replay are specified to reuse the **same reasoned
invariant family** already proven for history (§5 of
`ORUN_STORAGE_FLASH_OWNERSHIP.md`): body/CRC programmed first, a final
4-byte commit word programmed last and separately, so a torn write is always
distinguishable from a committed one by checking the commit word — never by
inferring completeness from body content alone. This ADR requires that any
future `ConfigStore`/`SecurityStore` implementation (§16) satisfy this same
invariant with its own dedicated pages and its own state, not by reusing
`HistoryStore`'s code or physical pages. Exact byte-level record layouts for
config/security are **not** designed in this ADR (per §4B/§4C — no
speculative struct), so no new power-cut fault-injection matrix can be
written yet; that follows in the implementation slice that defines each
format.

## 13. Migration/backward compatibility

- History's address (`0x0ED000..0x0F4000`) is **unchanged** by this ADR.
  A device already running firmware built before this ADR, receiving an
  update built after it (once §16's slices land), keeps its existing history
  content untouched — this ADR carves new pages from *unused application
  headroom*, not from history.
- If the application's effective ceiling is reduced per §6/§7 (soft build
  guard in M7P2), an **old firmware image** already flashed and running is
  unaffected (it was linked against the full `0x026000..0x0ED000` region and
  keeps running as compiled); the ceiling only constrains *future builds*.
  A future DFU/serial update replaces the whole application region in place
  (§14) — it does not need the new pages to exist beforehand, since they are
  simply left erased/unused by an old-image update and only become
  meaningful once new firmware that actually uses them is flashed.
- TLP v1/mixed-fleet impact: **NONE**, verified — this ADR does not touch
  `firmware/include/tlp_position_packet.h`, `tlp_relay_forward_packet.h`, RF
  parameters, or any wire codec. No protocol byte changes.
- No migration protocol is designed for config/security formats, because no
  implementation of either exists yet to migrate from — inventing one now
  would be exactly the "hayali dev struct" this ADR was told not to produce.

## 14. DFU interaction

- The recommended layout does not encroach on `0x0F4000..0x100000`
  (bootloader + settings) in any way — the new partitions sit strictly
  between `0x026000` and `0x0ED000`.
- If the M7P2 soft-ceiling guard is implemented, the **maximum future
  application image size for DFU purposes shrinks from 815,104 to 790,528
  bytes** (a 24,576-byte, ~3% reduction) — an explicit, computed
  consequence, not an afterthought. Current firmware (147,144 bytes) uses
  18.6% of the new ceiling, so this has no near-term practical effect.
- This ADR makes **no claim** that the physically flashed bootloader
  supports dual-bank DFU, validates image signatures, or has any particular
  rollback behavior — those remain the explicit `UNKNOWN`s recorded in
  `ORUN_STORAGE_FLASH_OWNERSHIP.md` §11/§14 (decision gate F), unresolved by
  this ADR and requiring physical verification as a separate task.
- `nrfutil`'s `--singlebank` flag (already unconditionally passed by
  PlatformIO's `nordicnrf52` builder for this board, per the audit) is
  unaffected by this ADR's address changes; DFU still writes only within the
  (now slightly smaller) application ceiling, using whatever bank behavior
  the actual bootloader implements.

## 15. Security ownership boundary

- This ADR allocates **storage and lifecycle ownership only**: two dedicated
  pages, an isolation boundary from config/bonds/history, and reset-policy
  rules (§11). It does not choose a cryptographic algorithm, key size, KDF,
  or provisioning transport.
- Anti-replay counters are explicitly **not** the history journal's sequence
  space (§3D) and must not be implemented by reusing `SequenceSource` or any
  history record type.
- No secret values, example keys, or key-shaped test fixtures are introduced
  by this ADR; there is no code in this change that could contain one.
- Any future implementation of this partition (§16, M7P5) must itself avoid
  inventing cryptography per `AGENTS.md`, and must specify anti-rollback
  behavior (a counter must never be observed to decrease across reboot,
  corruption-recovery, or firmware update) as part of that later design —
  not assumed satisfied by merely having two dedicated pages.

## 16. Implementation slices

Ordered; each is independently reviewable and should get its own PR. Naming
follows this repository's existing `M<n>P<n>`/lettered-slice convention
(e.g. `M6P1`, `M6B1`–`M6B3`):

- **M7P2 — Layout/build-guard implementation. DONE**, see
  `docs/milestones/M7P2.md`. Extended `check_storage_layout.py` with the
  soft application-size ceiling check (§6) and added the six new
  reserved-region constants (as inert, unused `constexpr` values — no
  backend code). Host tests and `pio run -e rak4630` pass unchanged; the new
  ceiling was verified to actually fire via a deliberate, uncommitted
  oversized local build.
- **M7P3 — SoftDevice-compatible async history flash backend.** Implement
  `FlashMutationGate` (§9) and the asynchronous `NrfHistoryFlash`-equivalent
  backend for `HistoryStore` only, proving the contract against the existing
  journal format before any new partition depends on it. Must not change
  `journal_format`'s on-flash layout.
- **M7P4 — BLE bond storage in its allocated partition.** Write and audit
  `patch_internalfs.py` (§8) relocating `LFS_FLASH_ADDR`/`LFS_FLASH_TOTAL_SIZE`
  to `0x0EB000`/`0x2000`, pinned against the exact upstream blob hash
  recorded in §8. No Bluefruit/bonding.cpp logic changes.
- **M7P5 — Durable config store.** Define the actual config schema (now,
  not speculatively) and implement `ConfigStore` in
  `0x0E9000..0x0EB000` using the A/B commit-word-last pattern from §7/§12.
- **M7P6 — Security material + anti-replay store.** Implement
  `SecurityStore` in `0x0E7000..0x0E9000`, with an explicit, reviewed
  cryptography/key-provisioning design as its own prerequisite (not
  authorized by this ADR) before any real key material is written.
- **M7P7 — BLE runtime/admission window.** Enable `Bluefruit.begin()` in
  production, gated on M7P3 (async flash) and M7P4 (relocated bonds) both
  being merged and validated; define the BLE advertising/connection
  admission policy itself (separate from storage) as its own design.
- **M7P8 — BLE DFU physical validation / implementation.** Requires the
  physical bootloader facts in decision gate F
  (`ORUN_STORAGE_FLASH_OWNERSHIP.md` §11/§14) to be resolved first; not
  gated on M7P2–M7P6 except for the application-ceiling change in §6/§14.

**Explicit dependency on future security/store-forward work**: any
authenticated-ACK/store-forward implementation (decision gate G,
`ORUN_STORAGE_FLASH_OWNERSHIP.md` §12/§16) that needs durable security
material or anti-replay counters must not begin before **M7P6** is merged
and validated. It must not be implemented against a "temporary" ad hoc
location, and must not reuse the history journal's sequence space (§3D).

## 17. Physical validation still required

Unchanged from `ORUN_STORAGE_FLASH_OWNERSHIP.md` and not resolved by this
design-only ADR:

- Actual bootloader dual-bank/rollback/image-validation capability (gate F).
- Real LittleFS per-file/per-directory metadata overhead on the actual
  bundled Adafruit_LittleFS version, to confirm the §3A bond-sizing estimate
  under real device firmware rather than by arithmetic alone.
- Electrical brownout/power-removal behavior at any new config/security
  partition's commit points, once implemented (mirrors the still-pending
  physical validation already noted for history).
- Whether the SoftDevice event pump's actual latency (`NRF_EVT_FLASH_OPERATION_*`
  arrival time under real BLE connection load) is compatible with
  `PositionFlow`'s live-TX timing expectations — an engineering question for
  M7P3, not answerable from source alone.

## 18. Decision status

| Decision | Status |
| --- | --- |
| Exact new partition addresses/sizes (§5, §7) | **DECIDED** |
| Bond backend (relocate `InternalFS` via core patch) | **DECIDED** |
| Config/security backend (raw A/B, journal-family pattern, not LittleFS) | **DECIDED** |
| Async flash contract shape (`FlashMutationGate`, single owner, bounded queue) | **DECIDED** (contract-level; not implemented) |
| Concurrency priority order (§10) | **DECIDED** |
| Reset/factory-reset policy per class (§11) | **DECIDED** |
| Application-ceiling enforcement mechanism (soft guard now, hard linker patch later) | **DECIDED** |
| Exact config schema | **NOT DECIDED** — deferred to M7P5, by design (§4B) |
| Cryptography/key scheme | **NOT DECIDED** — explicitly out of scope (§15) |
| Physical bootloader DFU capability (gate F) | **UNRESOLVED** — requires hardware, not decidable here |
| Authenticated ACK/store-forward protocol (gate G) | **UNRESOLVED** — gated on M7P6, not designed here |

This ADR resolves decision gate **E** from
`ORUN_STORAGE_FLASH_OWNERSHIP.md` §17. Gates **F** and **G** remain
`UNRESOLVED` and are not claimed resolved by this document.
