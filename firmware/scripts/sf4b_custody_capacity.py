#!/usr/bin/env python3
"""Deterministic SF4B gateway-custody capacity/wear planning model.

This is not a partition selector and contains no flash-endurance assumption.
It models the current SF4B candidate geometry under a specified retained-record
production cadence, assuming every distinct opaque object is committed by one
gateway. Byte-identical retries are deduped by CustodyStore and therefore add no
flash writes; reboot/re-protection can create opaque-distinct duplicates and is
modelled explicitly by opaque_duplicate_fraction.
"""

from dataclasses import dataclass

PAGE_BYTES = 4096
RECORD_BYTES = 88
PAGE_HEADER_BYTES = 64
RECORDS_PER_PAGE = (PAGE_BYTES - PAGE_HEADER_BYTES) // RECORD_BYTES
HOURS_PER_YEAR = 24 * 365

# Normal successful lifecycle programming, derived from the SF4B format/store:
# admission body+CRC (80) + commit (4); durable Edge handoff marker (4);
# prepared page header body (28) + commit (4) + activation (4);
# reclaim-intent body+CRC (16) + commit (4).
ADMISSION_PROGRAM_BYTES = 84
HANDOFF_MARKER_BYTES = 4
PAGE_HEADER_LIFECYCLE_BYTES = 36
RECLAIM_INTENT_BYTES = 20


@dataclass(frozen=True)
class Result:
    devices: int
    pages: int
    observations_per_device_hour: float
    opaque_duplicate_fraction: float
    region_bytes: int
    slots: int
    distinct_admissions_per_hour: float
    no_edge_fill_hours: float
    admissions_per_year: float
    page_reclaims_per_year: float
    erases_per_page_year: float
    admission_program_bytes_per_year: float
    handoff_marker_bytes_per_year: float
    page_header_program_bytes_per_year: float
    reclaim_intent_program_bytes_per_year: float
    total_program_bytes_per_year: float


def model(
    devices: int,
    pages: int,
    observations_per_device_hour: float,
    opaque_duplicate_fraction: float = 0.0,
) -> Result:
    if devices <= 0 or pages < 2 or observations_per_device_hour <= 0:
        raise ValueError("devices>0, pages>=2 and cadence>0 are required")
    if opaque_duplicate_fraction < 0:
        raise ValueError("opaque_duplicate_fraction must be >= 0")

    slots = pages * RECORDS_PER_PAGE
    produced_per_hour = devices * observations_per_device_hour
    distinct_admissions_per_hour = produced_per_hour * (
        1.0 + opaque_duplicate_fraction
    )
    admissions_per_year = distinct_admissions_per_hour * HOURS_PER_YEAR

    # In steady state, one physical page is reclaimed for each full page of
    # distinct committed custody records. This assumes downstream durable Edge
    # handoff eventually makes those records reclaimable; no Edge means fill
    # time below, then safe backpressure rather than overwrite.
    page_reclaims_per_year = admissions_per_year / RECORDS_PER_PAGE
    erases_per_page_year = page_reclaims_per_year / pages

    admission_bytes = admissions_per_year * ADMISSION_PROGRAM_BYTES
    handoff_bytes = admissions_per_year * HANDOFF_MARKER_BYTES
    page_header_bytes = page_reclaims_per_year * PAGE_HEADER_LIFECYCLE_BYTES
    reclaim_intent_bytes = page_reclaims_per_year * RECLAIM_INTENT_BYTES

    return Result(
        devices=devices,
        pages=pages,
        observations_per_device_hour=observations_per_device_hour,
        opaque_duplicate_fraction=opaque_duplicate_fraction,
        region_bytes=pages * PAGE_BYTES,
        slots=slots,
        distinct_admissions_per_hour=distinct_admissions_per_hour,
        no_edge_fill_hours=slots / distinct_admissions_per_hour,
        admissions_per_year=admissions_per_year,
        page_reclaims_per_year=page_reclaims_per_year,
        erases_per_page_year=erases_per_page_year,
        admission_program_bytes_per_year=admission_bytes,
        handoff_marker_bytes_per_year=handoff_bytes,
        page_header_program_bytes_per_year=page_header_bytes,
        reclaim_intent_program_bytes_per_year=reclaim_intent_bytes,
        total_program_bytes_per_year=(
            admission_bytes
            + handoff_bytes
            + page_header_bytes
            + reclaim_intent_bytes
        ),
    )


def main() -> None:
    cadence = 4.0  # 15-minute retained observation cadence.
    print(
        "devices,pages,region_kib,slots,no_edge_fill_hours,"
        "erases_per_page_year,total_program_mib_year"
    )
    for devices in (10, 30, 50, 100):
        for pages in (16, 24, 32):
            r = model(devices, pages, cadence)
            print(
                f"{r.devices},{r.pages},{r.region_bytes / 1024:.0f},"
                f"{r.slots},{r.no_edge_fill_hours:.2f},"
                f"{r.erases_per_page_year:.2f},"
                f"{r.total_program_bytes_per_year / (1024 * 1024):.2f}"
            )


if __name__ == "__main__":
    main()
