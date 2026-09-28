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
