# M6D3B physical qualification

Status: **IN PROGRESS — READ-ONLY PREFLIGHT PASS; DESTRUCTIVE A/B / REBOOT / POWER-CUT PENDING**.

Branch: `feat/m6d3b-geofence-store`.

Audited code-equivalent head: `bf1a753703c9a51381a0e0a31f631dba95a46c3d`.

Preflight execution branch head before upload:
`cb25e1189de48ddd50b3c1a26c7ec09d709c0fa5`.
The commits after the audited code head were documentation-only.

## Stage 1 — read-only geofence flash preflight

Target:

`rak4630_m6d3b_geofence_preflight`

Physical development unit: RAK4631 / RAK4630 target.

Observed serial evidence:

```text
M6D3B GEOFENCE PREFLIGHT BOOT
READ-ONLY: no flash program/erase path is linked into this image
REGION 0x0E5000..0x0E6FFF; qualification requires both pages all_ff=yes
M6D3B PREFLIGHT PAGE A inspect=PASS evidence=ERASED all_ff=yes tail_ff=yes crc32=F154670A
M6D3B PREFLIGHT PAGE B inspect=PASS evidence=ERASED all_ff=yes tail_ff=yes crc32=F154670A
M6D3B PREFLIGHT RESULT all_ff=yes action=QUALIFICATION_IMAGE_MAY_BE_USED
```

Result: **PASS**.

What this proves:

- physical page A at `0x0E5000..0x0E5FFF` is all `0xFF`;
- physical page B at `0x0E6000..0x0E6FFF` is all `0xFF`;
- both page tails are erased;
- the classifier sees both pages as ERASED;
- the reserved GeofenceStore partition is physically blank on this development
  unit before the first destructive geofence write.

What this does **not** prove:

- GeofenceStore physical write/commit correctness;
- A/B rollover;
- reboot persistence;
- physical power-cut recovery;
- SoftDevice-enabled flash arbitration;
- field geofence behavior.

The preflight image is structurally read-only and contains no linked geofence
flash mutation primitive/backend path, as independently audited before upload.

## Authorization boundary

Because both pages reported `all_ff=yes`, the destructive M6D3B qualification
image is now authorized on this exact development unit.

The next physical action must still be staged one step at a time. The first
destructive boot is expected to establish only the fresh authoritative CLEAR
baseline on page A using the real GeofenceStore / NrfGeofenceFlash / CSPRNG
path.

Do not claim A/B, reboot or power-cut PASS until their separate evidence is
recorded below.


### Reconnect / second-boot confirmation

The USB serial device disconnected and reconnected after the first observation.
On reconnect the same read-only image booted again and independently reported
the identical physical state:

```text
M6D3B PREFLIGHT PAGE A inspect=PASS evidence=ERASED all_ff=yes tail_ff=yes crc32=F154670A
M6D3B PREFLIGHT PAGE B inspect=PASS evidence=ERASED all_ff=yes tail_ff=yes crc32=F154670A
M6D3B PREFLIGHT RESULT all_ff=yes action=QUALIFICATION_IMAGE_MAY_BE_USED
```

This second boot strengthens only the read-only blank-partition evidence. It
does not constitute persistence, write, reboot-persistence or power-cut
qualification because the preflight image contains no mutation path.


## Stage 2A — first destructive boot / fresh CLEAR baseline

After the read-only preflight passed on the same development unit, the
`rak4630_m6d3b_geofence_qual` image was uploaded.

Observed serial evidence:

```text
M6D3B GEOFENCE QUAL BOOT
TEST-ONLY DESTRUCTIVE: use ONLY after read-only PREFLIGHT all_ff=yes
scope=0x0E5000..0x0E6FFF
M6D3B QUAL BEGIN PASS
M6D3B QUAL STORE ready=yes busy=no maintenance=no resource=CLEAR token_state=VALID mutations=0 failures=0 reconciliations=0
M6D3B QUAL TOKEN incarnation=0xD93BBFF182C898DC revision=1
M6D3B QUAL SNAPSHOT state=CLEAR areas=0 vertices=0
M6D3B QUAL PAGE A evidence=COMMITTED_CLEAR decoded=yes tail_ff=yes generation=0x0000000000000001 incarnation=0xD93BBFF182C898DC revision=1 state=CLEAR areas=0 vertices=0
M6D3B QUAL PAGE B evidence=ERASED decoded=no tail_ff=yes
M6D3B QUAL READY commands=STATUS,REPLACE,CLEAR,CLEAN
```

Result: **PASS** for the first physical fresh-baseline transaction.

This physically proves on this development unit:

- the blank geofence partition can initialize;
- a non-zero hardware-generated incarnation was obtained;
- the first durable token is revision 1;
- the baseline semantic state is CLEAR;
- page A was committed and decoded as `COMMITTED_CLEAR`;
- page A reserved tail remained erased;
- page B remained erased;
- GeofenceStore published VALID authority only after the committed record was
  visible and verified.

This still does **not** prove:

- A/B successor mutation;
- persistence across reboot;
- return mutation back to CLEAR;
- electrical power-cut recovery;
- SoftDevice-enabled arbitration.


## Stage 2B — physical A/B successor mutation

From the physically qualified fresh CLEAR baseline, the operator issued exactly
one `REPLACE` command using the test-only triangle fixture.

Observed serial evidence:

```text
M6D3B QUAL REPLACE result=CONFIRMED
M6D3B QUAL STORE ready=yes busy=no maintenance=no resource=CONFIGURED token_state=VALID mutations=1 failures=0 reconciliations=0
M6D3B QUAL TOKEN incarnation=0xD93BBFF182C898DC revision=2
M6D3B QUAL SNAPSHOT state=CONFIGURED areas=1 vertices=3
M6D3B QUAL PAGE A evidence=COMMITTED_CLEAR decoded=yes tail_ff=yes generation=0x0000000000000001 incarnation=0xD93BBFF182C898DC revision=1 state=CLEAR areas=0 vertices=0
M6D3B QUAL PAGE B evidence=COMMITTED_CONFIGURED decoded=yes tail_ff=yes generation=0x0000000000000002 incarnation=0xD93BBFF182C898DC revision=2 state=CONFIGURED areas=1 vertices=3
```

Result: **PASS** for the first physical A/B successor mutation.

This physically proves on this development unit:

- the inactive page B was used for the successor transaction;
- page A's previously committed CLEAR baseline remained intact;
- page B committed CONFIGURED generation 2 / revision 2;
- the incarnation remained unchanged across the semantic mutation;
- the complete configured snapshot contains one area / three effective
  vertices;
- both reserved tails remained erased;
- GeofenceStore published CONFIGURED with VALID token only after the new
  committed record was verified;
- no mutation failure or reconciliation was reported.

This still does **not** prove:

- recovery of the newer page after reboot;
- subsequent A/B rollover back to page A;
- physical electrical power-cut recovery;
- SoftDevice-enabled arbitration.


## Stage 2C — physical reboot persistence PASS

After the Stage 2B CONFIGURED state was committed, the qualification unit was
reset and the serial monitor reconnected to the same device.

Persistent USB identity was subsequently established as:

```text
/dev/ttyACM1
ID_SERIAL_SHORT=0E8ADE7E71531AA3
```

The operator confirmed that this was the device used for the qualification
upload and reboot.

Observed recovery after reboot:

```text
M6D3B QUAL BEGIN PASS
M6D3B QUAL STORE ready=yes busy=no maintenance=no resource=CONFIGURED token_state=VALID mutations=0 failures=0 reconciliations=0
M6D3B QUAL TOKEN incarnation=0xD93BBFF182C898DC revision=2
M6D3B QUAL SNAPSHOT state=CONFIGURED areas=1 vertices=3
M6D3B QUAL PAGE A evidence=COMMITTED_CLEAR decoded=yes tail_ff=yes generation=0x0000000000000001 incarnation=0xD93BBFF182C898DC revision=1 state=CLEAR areas=0 vertices=0
M6D3B QUAL PAGE B evidence=COMMITTED_CONFIGURED decoded=yes tail_ff=yes generation=0x0000000000000002 incarnation=0xD93BBFF182C898DC revision=2 state=CONFIGURED areas=1 vertices=3
```

Result: **PASS** for physical reboot persistence.

This physically proves on the qualified development unit:

- the newer CONFIGURED page B survives reset;
- boot recovery selects generation 2 / revision 2 as authority;
- the incarnation remains unchanged;
- the old committed CLEAR page A remains intact;
- token authority remains VALID;
- no maintenance/reconciliation path was needed.

This still does **not** prove:

- the next A/B rollover back to page A;
- physical electrical power-cut recovery;
- SoftDevice-enabled arbitration.


## Stage 2D — physical A/B rollover back to page A

Starting from the reboot-qualified CONFIGURED state on page B (generation 2 /
revision 2), the operator issued exactly one `CLEAR` command.

Observed serial evidence:

```text
M6D3B QUAL CLEAR result=CONFIRMED
M6D3B QUAL STORE ready=yes busy=no maintenance=no resource=CLEAR token_state=VALID mutations=1 failures=0 reconciliations=0
M6D3B QUAL TOKEN incarnation=0xD93BBFF182C898DC revision=3
M6D3B QUAL SNAPSHOT state=CLEAR areas=0 vertices=0
M6D3B QUAL PAGE A evidence=COMMITTED_CLEAR decoded=yes tail_ff=yes generation=0x0000000000000003 incarnation=0xD93BBFF182C898DC revision=3 state=CLEAR areas=0 vertices=0
M6D3B QUAL PAGE B evidence=COMMITTED_CONFIGURED decoded=yes tail_ff=yes generation=0x0000000000000002 incarnation=0xD93BBFF182C898DC revision=2 state=CONFIGURED areas=1 vertices=3
```

Result: **PASS** for physical A/B rollover back to page A.

This physically proves:

- the previously inactive page A was erased/reused for the next successor;
- generation advanced exactly `2 -> 3`;
- revision advanced exactly `2 -> 3`;
- the incarnation remained unchanged;
- page A became the new committed CLEAR authority;
- page B retained the previous committed CONFIGURED predecessor;
- both reserved tails remained erased;
- token authority remained VALID;
- no mutation failure or recovery reconciliation was required.

This closes the normal physical A/B alternation path for one complete
CLEAR -> CONFIGURED -> CLEAR cycle.

Still pending: focused physical electrical power-cut qualification.


## Stage 3A — power-cut probe pre-cut state verified

The focused test-only power-cut image was uploaded to the previously qualified
unit and boot/recovery was checked before arming any mutation.

Observed state:

```text
M6D3B POWERCUT STORE ready=yes busy=no maintenance=no resource=CLEAR token_state=VALID mutations=0 failures=0 reconciliations=0
M6D3B POWERCUT TOKEN incarnation=0xD93BBFF182C898DC revision=3
M6D3B POWERCUT SNAPSHOT state=CLEAR areas=0 vertices=0
M6D3B POWERCUT PAGE A evidence=COMMITTED_CLEAR decoded=yes tail_ff=yes generation=0x0000000000000003 incarnation=0xD93BBFF182C898DC revision=3 state=CLEAR areas=0 vertices=0
M6D3B POWERCUT PAGE B evidence=COMMITTED_CONFIGURED decoded=yes tail_ff=yes generation=0x0000000000000002 incarnation=0xD93BBFF182C898DC revision=2 state=CONFIGURED areas=1 vertices=3
```

Result: **PASS** for the pre-cut read-only recovery state.

This confirms the power-cut probe did not manufacture a baseline or mutate the
partition at boot. The authoritative state entering the electrical cut test is
page A CLEAR generation/revision 3 with the prior page B CONFIGURED
generation/revision 2.


## Stage 3B — deterministic pre-commit electrical cut point reached

From the verified CLEAR generation/revision 3 authority, the operator issued
exactly one `CUT_BODY` command.

Observed serial evidence:

```text
M6D3B POWERCUT CUT_BODY accepted candidate=CONFIGURED
M6D3B POWERCUT READY stage=after-body-readback before-commit; CUT POWER NOW
```

The READY line repeated continuously.

Result: **PASS** for reaching the intended deterministic cut boundary.

At this boundary the test-only wrapper has allowed the real inactive-page erase,
the real body+CRC program, the backend readback verification, and GeofenceStore's
own body readback/memcmp to complete. The exact 4-byte commit program has not
been delegated to the physical backend.

Electrical power removal and post-reboot recovery evidence are still pending.
