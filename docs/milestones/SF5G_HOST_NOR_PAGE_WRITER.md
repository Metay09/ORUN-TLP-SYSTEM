# SF5G host-only NOR writer: first executable durability slice

Date: 2026-10-13
Status: **CANDIDATE / NEW HEAD NOT YET TESTED / NO DEVICE FLASH OR GATEWAY RUNTIME**.
Parent: `main@60c47dea7c71b299afc33a59eea9ec5f54d941cf` (PR #120).

## Why this slice exists

PR #119 and #120 established host-only, unapproved 120-byte record and
OCP3/v3 page-header formats. They did **not** write records, read them
back, commit them on modeled NOR media, or simulate a restart after
power loss. This slice adds only that narrow behavior.

New files:

- `firmware/tests/sf5/sf5g_nor_page_writer_candidate.h/.cpp`
- `firmware/tests/sf5/test_sf5g_nor_page_writer_candidate.cpp`

The new executable is linked into `firmware/tests/run_host_tests.sh`
with the existing host `portable_flags` (ASan/UBSan and `-Werror`
when default sanitizers are enabled). Existing production sources,
flash ownership, radio/ACK paths, firmware images and connected hardware
are unchanged.

## Explicit model and safety properties

The `NorPage` object retains one 4,096-byte page across simulated
reset. A new `PageWriter` object represents reboot. NOR `program()`
accepts only 1->0 transitions; it can be interrupted after any attempted
byte. Read failure can be injected at any logical read operation. This
model has **no erase operation**.

The first write requires explicit `initializeEmpty(..., authorized=true)`
and proves that the **entire** 4 KiB page is erased before changing
anything. It writes/reads back the staged 64-byte OCP3 header, programs
and validates commit, then programs and validates activation. Any
foreign/legacy page, torn header, nonerased tail or read failure refuses
initialization. It never repairs or silently reformats uncertain media.

`appendExact()` accepts only the existing 84/100-byte PRODUCT_SECURE
custody shape. It insists on an ACTIVE header, an erased guard/intent
area, and ordered slots that are exclusively `HELD`, `HANDED_OFF`,
then `ERASED`. Any staged, torn, corrupt, noncontiguous or unknown slot
**blocks further appends** instead of being reused. It performs:

1. Full-page read and structural checks; exact-byte duplicate scan.
2. Encode/stage a 120-byte record (commit and handoff erased).
3. Read back all 120 bytes and require byte-for-byte match.
4. Program the separate 4-byte commit marker.
5. Re-read; require `kHeld` and the exact original packet bytes.

The resulting host-only `kStored` means the simulated page has
**structurally committed bytes**. It is **not** authorization for custody
ACK, not authenticated origin, and not proof of physical flash
durability. Full-byte duplicates produce `kDuplicate` locally without
rewriting flash, but do not trigger any ACK.

## Test coverage

- Every byte boundary of header stage/commit/activate (0..72) with
  independent reader after simulated power cut.
- Every byte boundary of record stage/commit (0..124); never promote or
  reuse a torn slot, and preserve an exact committed object on reboot.
- Read faults after initial census, staged write verification, and
  postcommit verification; no successful writer result after an
  unverified write.
- Explicit authorization denial, full-page erased check, OCQ1 refusal,
  foreign bytes and reserved gap pollution, NOR 0->1 rejection.
- Exact-byte duplicate after reboot, sequential page fill, and
  fail-closed `kFull` behavior with no erase.
- Two remaining nonblocking PR #120 LOW test gaps are closed:
  version=4 with untouched inverse v3 and fresh CRC, and positive
  allowed intent target pages 0 and 31.

## Deliberate restrictions and unresolved gates

This is a **single-page synchronous simulator**. It cannot recover or
reclaim a staged record: a byte-interrupted append after any actual
programming blocks future appends on that page. Likewise a partial
header may look `kForeign` and stop initialization. These are
intentional *safety over liveness* decisions, **not** solved recovery.
There is no multi-page census, rotation, erase, spare-page selection,
wear leveling, intent slot management, queue FIFO across pages,
asynchronous completion, physical flash timing, bit-granularity weak
program or interrupted erase model.

Before actual Gateway activation, implement and audit:

1. Product region ownership census, OCP3 vs legacy OCQ1/OBS1
   protection, torn-header repair policy and explicit authorized
   provisioning (never implicit erase).
2. Real asynchronous FlashBackend state machine with read-fault and
   power-cut injection at EVERY operation; no ACK before authenticated
   and verified durable custody.
3. Safe reclaim/rotation with exhausted intents and handoff slots
   (SF4B N1/N2), begin/reentrancy/read faults/scan timing (N3-N5),
   generation checks, no self-targeting and no lost held object.
4. End-to-end exact ciphertext cache on tracker, product family
   admission including delegated RESULT, complete Gateway custody ACK
   binding, and Gateway-to-Edge durability and deduplication.
5. Physical nRF52 measurements/qualification, battery evaluation and
   actual flash power-cut validation.

**This PR is NOT ready for merge until the new exact HEAD host suite
passes.** As agreed, reserve independent security audits for the later
storage-critical milestone rather than requiring one for each small
host-only commit.
