# SF4B Gateway CustodyStore — independent audit disposition

Status: **POST-AUDIT FIXES IMPLEMENTED; FRESH OWNER VALIDATION AND INDEPENDENT RE-AUDIT PENDING.**

Audited head:

```text
ea1085f132e6434b77e19de8fc49ec4337839861
```

Independent verdict:

```text
FAIL
BLOCKER 1
HIGH 2
MEDIUM 3
LOW 6
```

No physical gateway custody behavior was audited or claimed.

## B1 — stale historical reclaim intent could erase reused HELD custody

**Disposition: FIXED IN FORMAT/STORE V2, validation pending.**

Root cause: v1 had no durable completion marker. A historical committed intent
could remain on a previous append page forever. If the newer reused target
header later became corrupt, recovery could fall back to the previous page as
current and reinterpret the old intent as destructive authority.

Fix:

- reclaim intent moved to a 24-byte slot with an explicit completion word;
- successor PREPARED page commit/readback must complete first;
- completion word is then programmed/read-verified before maintenance success;
- recovery grants erase authority only to a committed **incomplete** intent on
  the current highest-generation ACTIVE page;
- completed and partial-completion intents never authorize erase;
- a valid reused target generation mismatch rejects destructive recovery.

Regression: the audit scenario is permanent in
`testStaleIntentCannotEraseReusedHeldPage()`.

## H1 — torn PREPARED header could become permanent global fault

**Disposition: FIXED IN FORMAT V2, validation pending.**

Root cause: v1 parsed magic/version/CRC before checking whether the header commit
word was authoritative. A torn body could therefore resemble corrupt or
unsupported format.

Fix:

- page-header classifier checks commit authority first;
- commit-erased header is STAGED regardless of torn body bytes;
- staged/partial/partial-activation page with a fully erased payload is a
  repairable blank;
- committed version is paired with its bitwise inverse before
  `kUnsupported` is accepted.

Regression: format classifier and
`testTornHeaderRepairableWithoutReboot()`.

## H2 — one torn intent slot could permanently stall steady-state reclaim

**Disposition: FIXED IN FORMAT V2, validation pending.**

Root cause: one intent slot lived in the page header. A staged/partial slot
could never be reused, while no reserve page existed at the point the intent was
needed.

Fix:

- the 72-byte page tail is now three independent 24-byte intent slots;
- staged/partial/completed slots are consumed and skipped;
- the next erased slot may authorize a new attempt;
- if all three are consumed before a reclaim can finish, maintenance returns
  explicit `kIntentSlotsExhausted`; no destructive guess is made.

Regression: `testMultipleIntentSlotsSurviveTornAttempts()`.

## M1 — FakeFlash allowed same-word rewrite unlike production Nrf backends

**Disposition: FIXED, validation pending.**

Fix:

- FakeFlash now rejects programming any destination byte that is not 0xFF;
- custody record grew from 88 to 92 bytes;
- two independent 4-byte handoff marker words are reserved;
- torn first handoff word remains HELD and retry uses the second erased word;
- if both marker words become unusable, the record remains HELD and the page is
  pinned rather than falsely reclaimed.

Regression: `testTornHandoffUsesSecondWord()`.

## M2 — failed mutation left cached RAM state stale until reboot

**Disposition: FIXED, validation pending.**

Fix:

- after any confirmed failure without an unreconciled backend mutation,
  `failCurrentJob()` performs read-only `recover()`;
- if the physical commit/activation actually landed, RAM roles are rebuilt from
  flash immediately;
- if the backend reports an unreconciled mutation, the store faults closed and
  does not attempt recovery while ownership is unknown.

Regression:

- `testActivationFailureReconcilesWithoutReboot()`;
- `testIntentCommitFailureReconcilesWithoutReboot()`;
- commit-landed admission recovery.

## M3 — SF4A boot/full-scan budget was missing

**Disposition: FIXED AS ANALYTICAL SOFTWARE BUDGET; target timing still a later runtime gate.**

The capacity script now reports worst-case admission full-scan and recovery
read/CRC work for 16/24/32-page planning cases. Current no-index behavior is
explicitly documented:

- admission duplicate scan is O(pages × records/page);
- recovery header uniqueness is currently O(pages²);
- no target-latency claim is made.

Before SF4D freezes ACK/rendezvous timing, real nRF52840 measurements must
decide whether this bounded scan fits the receive window or whether a small RAM
index is justified.

## Low findings

### L1 duplicate lookup read failure

Fixed: duplicate lookup is tri-state; read/corrupt evidence returns
`kRejected` instead of "not found".

### L2 count/query read failures

Fixed: `heldCount` / `handedOffCount` return success + out-count, and
`oldestHeld` returns FOUND/NONE/READ_ERROR.

### L3 begin during unreconciled async mutation

Fixed: `begin()` rejects/faults closed when
`hasUnreconciledMutation()` is true.

### L4 prepared/intent ordering

Addressed by commit-first header classification and v2 completion semantics.

### L5 capacity/wear nuance

Model now includes explicit late-retry readmission, opaque-distinct duplicate
factor, pinned-page erase concentration and analytical scan work. Exact
nRF52840 n_WRITE/endurance values remain intentionally unclaimed until verified
against the selected hardware source.

### L6 test gaps

Added coverage for torn header/intent phases, async erase, read failures,
unreconciled begin, stale handle/page generation reuse, generation overflow,
commit-landed failures and the B1/H1/H2 probe scenarios.

## Format-v2 geometry

```text
page:          4096 bytes
header:          64 bytes
record:          92 bytes
records/page:    43
record bytes:  3956
reserved gap:     4 bytes
intent slots: 3 × 24 = 72 bytes
```

One record remains bound to the current exact 73-byte HISTORY_SECURE
observation object.

## Evidence boundary

These changes do not add:

- physical custody partition;
- `NrfCustodyFlash`;
- production CustodyStore instance;
- gateway RF runtime;
- custody ACK wire/MAC/KDF;
- tracker release mutation;
- Edge/backend transport;
- physical power-cut qualification.

The next evidence gates are exact-head aggregate host tests, normal RAK4630
build, then focused independent re-audit.
