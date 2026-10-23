# SF5K3I — live SecurityStore v3 recovery and counter ownership

**Status: DRAFT, not yet host-compiled/tested or target-compiled for this branch.**
Stacked on SF5K3H draft PR #145. No change to `main` and no real
device firmware upload.

## What actually changed

This is the first SF5K3 product slice in which the **real
`SecurityStore`** can recover a physically committed v3 page and
continue issuing TX counter blocks and persisting authenticated
A2D replay bounds through 56-byte v3 records. It uses the same
existing SecurityStore `FlashBackend`/FlashMutationGate views.

- On boot, SecurityStore detects v3 physical headers and uses
  `security_v3_ab_preflight::inspect` for fail-closed two-page
  selection and cross-page antirollback checks. It independently
  replays the selected v3 journal using bounded streaming reads,
  validates exactly matched credential/epoch/counter/policy scope,
  and counts burned slots before any append.
- An entirely erased page header with non-erased credential or
  journal bytes is now `kFault` even in legacy recovery. This
  prevents silent resurrection of old v2 credentials when newer
  v3's header disappears during a torn erase.
- On a committed v3, reboot **burns unused TX numbers** by setting
  the in-RAM next counter to the durable exclusive bound. Reboot
  similarly burns A2D counters below the durable bound.
- v3 TX / A2D state appends program/verify 52-byte body and then
  4-byte commit via the existing SecurityStore async/pending
  operation handling. A failed v3 append now halts protected
  service until reboot, because a physically successful but
  ambiguously acknowledged commit may have advanced the strict
  exclusive bound and must never be retried at an equal value.
- `requestV3Migration()` is a **local explicitly invoked
  maintenance method**, not a boot action or BLE/RF command.
  It requires an idle provisioned v2 page and an independently
  validated same-credential unchanged-root snapshot.
  `SecurityStore::poll` actually drives SF5K3H's commit-last
  physical A/B writer and transitions to v3 ONLY after verified
  activation. A failed/ambiguous transition locks the current
  SecurityStore instance; reboot must arbitrate the flash state.
- After v3 log approaches capacity, the same SecurityStore
  automatically compacts v3→v3 via its owned SF5K3H writer. The
  entire Gateway policy and four per-slot revocation tombstones
  are copied unchanged, never projected into v2.
- New **credential provisioning** still writes v2; existing
  automatic v1→v2 migration remains unchanged. Credential
  rotation while v3 is active deliberately fails closed
  until an explicitly reviewed v3 credential-rotation protocol
  is designed. No automatic v2→v3 field migration occurs on boot.

## Security boundaries

- Gateway IDs in committed v3 policy are recovered and preserved
  **internally**, but **NO new grant/revoke admission API** has
  been added. Reading a policy snapshot alone does not authorize
  a radio sender, Gateway ACK, record release or BLE administrator.
- No ACK/KDF/RF dispatch or ProductRecordStore release path is
  changed. Relay remains forward-only; it cannot acknowledge data.
- A page-writer success is only a physical durability result,
  not cryptographic Gateway receipt or backend custody.
- Source v2 pages are preserved during migration (writer erases
  only inactive target), allowing fail-closed recovery from
  interrupted pre-activation state.
- Physical FlashMutationGate ownership is still required; this
  change does not make an arbitrary caller safe to write while
  another security writer is in flight.

## Real SecurityStore host regression

`firmware/tests/sf5/test_sf5k3i_real_security_store_v3.cpp`
exercises, on an exact two-page fake NOR backend:
- New v2 credential, A2D reserve, explicit v2→v3 migration,
  physical A/B selection and unchanged source bytes
- Actual SecurityStore reboot to v3, TX reserved-number burn,
  A2D replay reject/advance and physical v3 append
- Migration failure with staged header and source-v2 recovery
- Ambiguous v3 counter COMMIT physically landing after a failed
  backend return: no same-boot replay, secure reboot recovery
- Failure of the newest v3 header erase to ever resurrect v2
- Independently encoded v3 Gateway grant/revocation tombstone
  surviving live TX and A2D appends, v3 full-log compaction,
  and reboot with no created Gateway ACK admission

Legacy security-store tests' unknown-future version fixture
has been shifted from v3 to v4. The older SF5K3E diagnostic
test now confirms inspection remains read-only even though
the actual v3 SecurityStore can recover v3. Both focused
and full host runners link the new production source files.

## Validation / release blockers

At time of this document, no new SF5K3I host PASS or RAK4630
SUCCESS has been observed; all described checks are test
implementations, NOT claims that they passed.

For iterative validation, first run ONLY the new actual-Store
integration scenario (the older SF5K3A-H tests need not be repeated):

```bash
cd ~/ORUN-TLP-SYSTEM
bash tools/ci/run_sf5k3i_integration_host.sh
```

After this focused gate passes, full host regression and the target
build are release gates, preferably automated by GitHub CI rather
than repeated manually after each small edit:

```bash
bash firmware/tests/run_host_tests.sh
cd firmware && pio run -e rak4630
```

The earlier SF5K3H PR #145's GitHub Actions failure has
not been explained (workflow logs were unavailable), so
GitHub CI remains an unresolved release gate.

**Do not merge, flash field devices, or enable Gateway ACK/
record release until compiled/host qualified and the v3
FlashMutationGate physical timing, power cut and credential
rotation constraints receive review.**
