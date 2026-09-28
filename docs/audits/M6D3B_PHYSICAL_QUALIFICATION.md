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
