# Step 3B — signed, persistent Gateway grants (OGK1 over USB)

**Status:** code + host tests. **Physical check: pending (owner).**
Plan: `docs/architecture/ORUN_STEP3_REAL_SECURITY_PLAN.md`. Base: step 3A.
No radio, ACK, record or phone format changes. The Gateway's custody ACK keys
are the same step 3A keys. What changes is where they come from and where
they live.

## What changes

| | Step 3A | Step 3B |
| --- | --- | --- |
| Gateway custody ACK keys | `BENCH GRANT` lines into RAM, lost on reboot | one snapshot signed by the backend issuer, kept in the Gateway's security region |
| Who decides | whoever has USB access | USB access delivers the bytes. The pinned issuer signature, the Gateway id and a higher generation decide whether they are installed |
| After a Gateway restart | run `orun_provision.py gateway` again | nothing to do |

## Pieces

### Backend (`backend/orun_credentials.py`)

- One Gateway **issuer** key (Ed25519), stored in the registry (0600). It is created once and never replaced.
- Every Gateway has a snapshot `generation`. It only grows, and it never falls below what the Gateway reports (`at_least`).
- `build_ogk1()` makes the exact SF5K3Y OGK1 page:
  - a 32-byte header: magic `OGK1`, version 1, 32 slots, Gateway id, generation, and at offset 24 the issuer key number (1);
  - 32 × 56-byte slots, sorted by Tracker id: tracker, epoch, floor, grant generation, flags = 1, scoped key;
  - an Ed25519 signature over the 1824 canonical bytes;
  - a CRC-32 and the commit word 0.
- Slots hold every *active* grant of that Gateway. A retired (re-keyed) Tracker credential drops out of the next snapshot, so a newer snapshot revokes it.
- `ogk1_upload_lines()`: `BENCH OGK1 BEGIN`, 30 × `BENCH OGK1 DATA <offset> <64 bytes hex>`, then `BENCH OGK1 COMMIT <crc32>`. Each line is at most 149 characters, inside the 160-byte command buffer.

### Build (`firmware/scripts/step3b_issuer_key.py`, Gateway bench env only)

- Pins the issuer **public** key from `~/orun-issuer.pub` (or `$ORUN_ISSUER_PUBLIC_KEY_FILE`). It goes into a generated header.
- Without that file the build stops and names the command that creates it (`orun_provision.py issuer`). `ORUN_STEP3B_ALLOW_NO_ISSUER=1` builds an image that accepts no snapshot (`BENCH?` shows `status=NO_ISSUER_KEY`).
- The key is never taken from USB, BLE or radio.

### Gateway bench image (`-DORUN_STEP3B_GATEWAY_ENROLLMENT`)

- The security region (0xE7000–0xE9000) belongs to the Gateway's OGK1 snapshot. `main.cpp` does not mount the Tracker SecurityStore on this image.
- Boot, before SoftDevice:
  - `nRFCrypto.begin()` starts CC310. Bluefruit's own call later is then a no-op, so there is one lifecycle.
  - The unmodified SF5K3Y `GatewayGrantSnapshotReader` reads the region with the SF5K3AA `GatewayEd25519Cc310Verifier` and the pinned key.
  - `READY` → the bench grant chain asks the signed snapshot first, then the step 3A RAM table, then the public test key.
- `BENCH OGK1 …` lines go to `Step3bGrantInstaller` (`step3b_gateway_enrollment.{h,cpp}`). Each line is wiped after use.
- **Checks before anything is written:**
  1. complete image;
  2. the transport CRC;
  3. the Gateway id;
  4. the installed region is `READY` or blank — an unknown, foreign (ORS1) or torn region is never overwritten;
  5. the candidate passes the same reader logic with the same pinned verifier, over a read-only RAM view;
  6. its generation is strictly higher than the installed one.
- **Writing** (same discipline as the SF5K3AC simulator):
  1. deny the boot snapshot for this boot;
  2. erase only the *other* page, and check it is blank;
  3. program the body in 64-byte chunks, reading each one back;
  4. read the whole body back again and check the commit word is still erased;
  5. program the commit word, then read the whole page back;
  6. reboot.

  An asynchronous completion is polled and never resubmitted. Any error latches `FAULT`, and the old snapshot stays denied until reboot.
- `BENCH?` adds:

  ```
  BENCH OGK1 status=… generation=… issuer=<first 4 bytes> installer=… signed_acks=…
  ```

  The issuer field holds only public bytes.

### Tool (`tools/orun_provision.py`)

- `issuer`: creates the issuer key once. It writes the public key to `~/orun-issuer.pub` and prints its first 4 bytes.
- `gateway`:
  1. reads `BENCH?`;
  2. refuses a Gateway built without the issuer key or with another one, a foreign or unreadable region (→ `CLEAN` with the cleanup image), or a writer that already ran this boot;
  3. signs the snapshot with a generation above the Gateway's and uploads it;
  4. waits for `COMMIT PASS`, reopens the port after the reboot and checks `status=READY` with the new generation.
- `gateway --ram`: the step 3A RAM path, kept as a fallback.

## Documented deviations / limits

- **Rollback anchor.** The anchor is the snapshot already on the Gateway, not an independent monotonic checkpoint (SF5K3Z). A blank region accepts its first snapshot.
  - Someone with USB access can `CLEAN` the region and install an older *genuine* snapshot.
  - USB access is the bench's provisioning authority (ADR M7P6 §11), so this is accepted for the bench. Field Gateways need the SF5K3Z anchor.
- **Power cut during the ~1 s write.** A power cut while the new page is being written leaves a torn page. The reader refuses the whole region at the next boot: the Gateway falls back to RAM grants or the test key, and nothing is wrongly authorized. Recovery is `CLEAN` (cleanup image), then `orun_provision.py gateway` again.
- **32 Trackers per Gateway** (OGK1 slots). One epoch per Tracker per snapshot.
- **Bench grant chain.** It still contains the step 3A RAM table and the public test key fallback. They are removed in 3D.
- **CC310 Ed25519 on the target.** It is proven only by this physical check. The SF5K3AA KAT image (`rak4630_sf5k3aa_ed25519_probe`) is the fallback diagnosis if `status=REJECTED` appears on a genuine snapshot.

## Evidence

- `test_step3b_gateway_enrollment.cpp` (ASan/UBSan), driven by the Python builder's exact USB lines (`--vectors`):
  - the read-only staging view;
  - line parsing (order, offsets, hex, lowercase refused, overflow);
  - first snapshot on a blank region;
  - a higher generation goes to the other page and leaves the old page intact;
  - revocation by a newer snapshot without the Tracker;
  - every slot key equals the backend's (= the step 3A vector);
  - sync and async flash;
  - refusals with flash byte-for-byte untouched and the boot snapshot still usable: rollback, equal generation, wrong Gateway, wrong issuer, transport CRC, tampered key byte with and without a fixed OGK1 CRC, commit word not 0, foreign ORS1 region, unreconciled flash;
  - flash failure at every step → FAULT, denied, `BEGIN` refused until reboot, the torn page never trusted at the next boot;
  - an earlier byte disturbed before commit → no commit;
  - grant chain order: signed, then RAM, then test key, and denied → RAM.
- `test_step3b_gateway_grants.py`:
  - exact layout and signature, the 32-slot limit and invalid grants, upload line limits;
  - the issuer is created once, generations only grow and respect `at_least`, a retired credential leaves the snapshot;
  - the tool against a scripted Gateway: success with reboot and check, and every refusal sends no OGK1 line;
  - the build pre-script (pinned / missing stops the build / explicit no-key image / invalid);
  - the source contract: no forbidden class names in `main.cpp`, the flag only in the Gateway bench env, deny before the first mutation.
- Full host suite PASS.
- Builds:
  - `rak4630` unchanged (362 912 B);
  - Gateway bench 397 484 B with a key, 385 436 B without;
  - the Tracker bench, the SF5K3AA probe and the cleanup image build.
