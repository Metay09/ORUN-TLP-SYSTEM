# SF5G — Host-only multi-page NOR progression

Status: **CANDIDATE / TESTS PENDING / NO GATEWAY OR PHYSICAL FLASH**.
Parent: `main@6feb119751bbfb68dbdd2c4307d41c03ef0096e3`
(PR #121 verified and merged).

## Current slice

The prior single-page `NorPage`/`PageWriter` model can prove a staged
write is re-read and committed, but cannot advance when full. This slice
adds `RegionMedia` and `RegionWriter` only under `firmware/tests/sf5`.

* Configurable host-only 1..32 simulated 4 KiB pages, retained across
  constructing a fresh RegionWriter (reboot).
* Full census **before every** initialization and append. Every physical
  page must be one of: a contiguous prefix of valid OCP3 ACTIVE pages
  with generation values 1,2,...,N; or a fully erased suffix.
* A newer active page is valid **only when every older active page is
  full**. Foreign/legacy/torn/partially committed pages, gaps,
  duplicate/out-of-order generations, unexplained bytes, or read failures
  refuse the *whole* region. There is no automatic migration or erase.
* Explicit owner authorization on each mutation. Initial page 0 is
  opened only with authorization and a fully erased region.
* Appending uses the existing exact-object record writer and keeps all
  prior bytes. Once the most recent page is full, the next completely
  erased page receives generation+1 via the existing staged/readback/
  commit/readback/activate sequence. Then append proceeds.
* Duplicates are sought across **all** pages by comparing the exact
  84/100 protected bytes, including entries marked handed off. This
  remains a structural host-only suppression, **not** authenticated
  sender identity, durable custody proof or ACK authority.
* On exhausting the configured page count, return `kFull`, never erase.
  This is append-only progression, **not reclamation**.

## Injected failure tests

The host runner adds `test_sf5g_region_writer_candidate.cpp`:

* 3-page sequential fill (96 records) then fail-closed `kFull`,
  with exact-byte duplicate detection after reconstructing RegionWriter.
* Owner not authorized, foreign SF4B page, noncontiguous pages,
  newer page before older full, wrong generation, read fault, and
  zero-page invalid region.
* Rotation between page 0 and page 1 interrupted after **each byte
  of the header and first record** (budgets 0..196). After reboot:
  at 0 bytes, retry cleanly; at completed header 72 bytes, continue
  with an empty active page; at 196 bytes, recover the exact committed
  record; all nonempty partial writes fail closed and cannot be reused.

This model does not simulate every NOR bit-tear, erase interruption,
asynchronous callback, concurrent reader, or write verification of a
physical FlashBackend. No new production source is linked and no
RAK4630 flashing occurs.

## Safety and remaining product requirements

* **Liveness remains intentionally limited.** A torn/foreign page
  blocks mount and append rather than being implicitly erased. A safe
  plan to classify/retire/recover those pages without losing HELD
  observations is still required.
* **No reclaim or rotation back to reused pages**: reclaim-intent
  consumption and exhaustion (SF4B N1), double handoff markers (N2),
  reentrant async begin (N3), read errors (N4), measured scan timing (N5),
  wear leveling and spare pages require independent engineering.
* Physical product-region ownership is decided by D2, not by this
  simulator's caller-controlled `owner_authorized` flag. Future Gateway
  activation must ensure an actual Gateway job, recognize OCP3/legacy
  formats, preserve foreign pages, and forbid self-targeted reclaim.
* The parser and full-byte duplicate check do not authenticate AEAD.
  Gateway acceptance requires cryptographic verification and durable
  readback evidence. **There is no ACK API and zero authorization for
  custody ACK** in this slice.
* The tracker exact-sealed-frame cache, RESULT-family admission,
  Gateway/Edge durable transfer, power-cut physical tests and
  asynchronous hardware FlashBackend remain unfinished.

Verification must be rerun on the exact newest branch commit.
Because this is host-only and does not affect physical media or ACK,
we will not demand a new independent audit for each minor host change.
Audit the bounded storage-critical integration milestone before runtime.
