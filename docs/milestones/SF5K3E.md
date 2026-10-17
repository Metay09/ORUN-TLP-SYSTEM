# SF5K3E — SecurityStore v3 read-only integrity inspection (draft)

**Status:** isolated product-source diagnostic reader and new host tests;
not yet qualified by a user-host run, hardware build or physical flash test.
This is deliberately *not* the actual v3 migration or persistent writer.

## Boundaries

This change lives in a separate branch / draft PR layered on top of the
verified SF5K3A–D model. It adds
`SecurityStore::inspectV3PageReadOnly(page, summary)` to the real
`firmware/include/security_store.h` and
`firmware/src/security_store.cpp`.

The existing `SecurityStore::begin/recover/poll` path is UNCHANGED.
It continues to recognize **only v1/v2 as security authority** and still
writes v2. A committed v3 page remains `SecurityState::kUnsupported`;
the inspector **never** sets `state_`, `credential_`, `tx_next_`,
`a2d_replay_bound_`, `pages_`, any Gateway grant, or `job_`.
No RF/USB/BLE runtime calls this diagnostic yet. It never invokes
`FlashBackend::program`, `erasePage` or `pollPending`.
It returns no `K_root` or Gateway ID.

## Inspection contract

The scan is bounded to two pages, a 32 B header, a 68 B credential,
71 × 56 B v3 records and a 20 B erased tail, consistent with SF5K3C/D.
All reads go through the store's existing FlashBackend. After `begin()`,
inspect each page independently:

- Blank page or intact older version: `kNotV3`
- Recognized v3 header with erased activation: `kStaged`
- Activated and fully validated page: `kVerified`
- Intact v3 page belonging to another Tracker: `kForeign`
- CRC/commit/semantic mismatch, missing required snapshot entry, old
  generation reuse, policy revision/floor rollback, hole, unknown type,
  reserved-byte mutation or dirty tail: `kCorrupt`
- Flash read fault, out-of-range index or premature pre-begin access:
  `kReadFault`

Only `kVerified` publishes a bounded scalar summary: page generation,
credential epoch (no secret), TX/A2D exclusive bounds, custody floor and
policy revision, active Gateway count, maximum grant generation. Neither
raw Gateway identities nor root secrets are returned. All failures leave
the caller's summary untouched.

The **mandatory first five v3 records** must be an exact credential/epoch-
bound policy snapshot plus four ordered slots. Read-only parsing validates
cross-slot uniqueness, revocation tombstones, global generation high-water,
nondecreasing floor, exact +1 revisions and the counter block sizes.
There are no hooks that interpret these fields as production authorization.

## Host evidence requested

`firmware/tests/sf5/test_sf5k3e_real_security_v3_readonly.cpp` runs against
the REAL `SecurityStore` with a controlled in-memory FlashBackend and
the independent SF5K3C/D fixture builder. It checks a valid v3 page
stays unsupported and cannot cause writes, counter reservation or root
readback; valid summary; staged/foreign/corrupt/flash-fault/empty and v2
cases; incomplete prefix; credential scope; reserved tail; policy/generation
rollback and malformed post-snapshot appends.

Run from repo root on this feature branch:

```bash
bash tools/ci/run_sf5k3a_host.sh
bash firmware/tests/run_host_tests.sh
cd firmware && pio run -e rak4630
```

**No claim of PASS at this commit until those execute.** These tests do
not prove flash-driver timing, SoftDevice arbitration, hardware reboots,
nor v3 A/B protection against rollback of an older valid page after
power loss.

## Next security gate

The next separate step is a *reviewed, durable* v3 two-page recovery and
snapshot writer, with writer-owned activation, exact A/B generation
checks, power-loss cuts, and verified credential/grant rotation.
Even a successfully inspected page must NEVER become live authority merely
because its CRC and layout parse. Custody enrollment, isolated KDF/HMAC,
Gateway durable receipt and record release are further blocked.
