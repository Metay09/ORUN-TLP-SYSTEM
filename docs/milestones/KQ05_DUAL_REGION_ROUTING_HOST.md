# KQ05 128 KiB + 28 KiB offline dual-region routing (host-only)

**Status: PORTABLE DECISION TEST, NOT an enabled second Flash writer.**

The role-aware 28 KiB census (PR #336) passed on the user's PC.
This slice adds the next bounded source component:
`kq05_dual_region_decisions.h`.

## What is modeled and checked

- A Tracker/Gateway keeps the existing 128 KiB as the primary exact-owner
  store; supplemental 28 KiB is an entirely separate region.
- Normal append goes to primary when healthy. Only when it is full, AND
  legacy History is disabled, AND supplemental role/device binding is
  trusted, may a supplemental write be *selected* (not performed).
- A Tracker's supplemental write is additionally forbidden until a shared
  incarnation/sequence authority spans both physical stores. Naively
  calling two independent ObservationStore instances is NOT safe.
- Before any new write, independent duplicate checks covering both stores
  must be exact and conclusive. An uncertain read or conflicting identity
  never gets a second append permit. Even a positive supplemental duplicate
  must not be trusted until the supplemental owner binding is proven.
- Authenticated release routing targets exactly one record owner: primary
  OR supplemental. Unauthenticated, wrong-scope, uncertain and dual-present
  records are blocked. No independent release API is introduced.
- This is **not** the Gateway BATCH_CUSTODY_ACK protocol and has no
  cryptographic verification; boolean inputs are only portable host models.
  A production integration must derive them from trusted local services.

## What is still missing

1. Actual exclusive role-bound physical FlashMutationGate port for 28 KiB,
   with recoverable async commit, no overlap, and explicit legacy cleanout.
2. Tracker shared sequence/incarnation allocator and cross-store exact
   cache/retention/release lifecycle.
3. Gateway credential-bound OCQ2 supplemental ownership and cross-store
   duplicate lookup, no-eviction custody + authenticated handoff.
4. Versioned selective authenticated Gateway ACK (up to 8) with tests
   covering multiple independent offline Gateways and delayed RF.
5. Full isolated PC regressions, physical power-cycle/radio trials and
   authorized device migration. No automatic erase, no firmware upload.

Host gate:
`bash tools/ci/run_kq05_dual_region_decisions_host.sh`

The decision test alone cannot prove usable **156 KiB** or extra records;
those remain integration goals rather than verified hardware capacity.
