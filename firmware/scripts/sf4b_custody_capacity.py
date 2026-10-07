#!/usr/bin/env python3
"""Deterministic SF4B gateway-custody capacity/wear/scan planning model.

This is not a partition selector and contains no flash-endurance assumption.
Normal-write arithmetic follows CustodyStore format v2. Byte-identical retries
are deduped while their exact object remains retained and readable; late retries
after reclaim and tracker reboot/re-protection can consume new custody slots, so
both are explicit input factors rather than hidden assumptions.
"""

from dataclasses import dataclass

PAGE_BYTES = 4096
PAGE_HEADER_BYTES = 64
RECORD_BYTES = 92
RECORDS_PER_PAGE = 43
INTENT_SLOT_BYTES = 24
INTENT_SLOTS_PER_PAGE = 3
INTENT_AREA_BYTES = INTENT_SLOT_BYTES * INTENT_SLOTS_PER_PAGE
HOURS_PER_YEAR = 24 * 365

ADMISSION_PROGRAM_BYTES = 84
HANDOFF_MARKER_BYTES = 4
PAGE_HEADER_LIFECYCLE_BYTES = 40
RECLAIM_INTENT_BYTES = 24
RECORD_CRC_BYTES = 76


@dataclass(frozen=True)
class Result:
    devices: int
    pages: int
    observations_per_device_hour: float
    opaque_duplicate_fraction: float
    late_retry_readmission_fraction: float
    pinned_pages: int
    region_bytes: int
    slots: int
    distinct_admissions_per_hour: float
    no_edge_fill_hours: float
    admissions_per_year: float
    page_reclaims_per_year: float
    cycling_pages: int
    erases_per_cycling_page_year: float
    admission_program_bytes_per_year: float
    handoff_marker_bytes_per_year: float
    page_header_program_bytes_per_year: float
    reclaim_intent_program_bytes_per_year: float
    total_program_bytes_per_year: float
    duplicate_scan_header_reads: int
    duplicate_scan_record_reads: int
    duplicate_scan_read_bytes: int
    duplicate_scan_crc_bytes: int
    recovery_header_reads_worst_case: int
    recovery_record_reads_worst_case: int
    recovery_read_bytes_worst_case: int
    recovery_crc_bytes_worst_case: int


def model(
    devices: int,
    pages: int,
    observations_per_device_hour: float,
    opaque_duplicate_fraction: float = 0.0,
    late_retry_readmission_fraction: float = 0.0,
    pinned_pages: int = 0,
) -> Result:
    if devices <= 0 or pages < 2 or observations_per_device_hour <= 0:
        raise ValueError("devices>0, pages>=2 and cadence>0 are required")
    if opaque_duplicate_fraction < 0 or late_retry_readmission_fraction < 0:
        raise ValueError("duplicate/readmission fractions must be >= 0")
    if pinned_pages < 0 or pinned_pages >= pages:
        raise ValueError("pinned_pages must be in [0, pages)")

    slots = pages * RECORDS_PER_PAGE
    produced_per_hour = devices * observations_per_device_hour
    distinct_admissions_per_hour = produced_per_hour * (
        1.0 + opaque_duplicate_fraction + late_retry_readmission_fraction
    )
    admissions_per_year = distinct_admissions_per_hour * HOURS_PER_YEAR
    page_reclaims_per_year = admissions_per_year / RECORDS_PER_PAGE
    cycling_pages = pages - pinned_pages
    erases_per_cycling_page_year = page_reclaims_per_year / cycling_pages

    admission_bytes = admissions_per_year * ADMISSION_PROGRAM_BYTES
    handoff_bytes = admissions_per_year * HANDOFF_MARKER_BYTES
    page_header_bytes = page_reclaims_per_year * PAGE_HEADER_LIFECYCLE_BYTES
    reclaim_intent_bytes = page_reclaims_per_year * RECLAIM_INTENT_BYTES

    duplicate_scan_header_reads = pages
    duplicate_scan_record_reads = pages * RECORDS_PER_PAGE
    duplicate_scan_read_bytes = (
        duplicate_scan_header_reads * PAGE_HEADER_BYTES
        + duplicate_scan_record_reads * RECORD_BYTES
    )
    duplicate_scan_crc_bytes = duplicate_scan_record_reads * RECORD_CRC_BYTES

    recovery_header_reads_worst_case = pages * pages + pages
    recovery_record_reads_worst_case = pages * RECORDS_PER_PAGE
    recovery_read_bytes_worst_case = (
        recovery_header_reads_worst_case * PAGE_HEADER_BYTES
        + recovery_record_reads_worst_case * RECORD_BYTES
        + INTENT_AREA_BYTES
    )
    recovery_crc_bytes_worst_case = (
        recovery_record_reads_worst_case * RECORD_CRC_BYTES
    )

    return Result(
        devices=devices,
        pages=pages,
        observations_per_device_hour=observations_per_device_hour,
        opaque_duplicate_fraction=opaque_duplicate_fraction,
        late_retry_readmission_fraction=late_retry_readmission_fraction,
        pinned_pages=pinned_pages,
        region_bytes=pages * PAGE_BYTES,
        slots=slots,
        distinct_admissions_per_hour=distinct_admissions_per_hour,
        no_edge_fill_hours=slots / distinct_admissions_per_hour,
        admissions_per_year=admissions_per_year,
        page_reclaims_per_year=page_reclaims_per_year,
        cycling_pages=cycling_pages,
        erases_per_cycling_page_year=erases_per_cycling_page_year,
        admission_program_bytes_per_year=admission_bytes,
        handoff_marker_bytes_per_year=handoff_bytes,
        page_header_program_bytes_per_year=page_header_bytes,
        reclaim_intent_program_bytes_per_year=reclaim_intent_bytes,
        total_program_bytes_per_year=(
            admission_bytes + handoff_bytes
            + page_header_bytes + reclaim_intent_bytes
        ),
        duplicate_scan_header_reads=duplicate_scan_header_reads,
        duplicate_scan_record_reads=duplicate_scan_record_reads,
        duplicate_scan_read_bytes=duplicate_scan_read_bytes,
        duplicate_scan_crc_bytes=duplicate_scan_crc_bytes,
        recovery_header_reads_worst_case=recovery_header_reads_worst_case,
        recovery_record_reads_worst_case=recovery_record_reads_worst_case,
        recovery_read_bytes_worst_case=recovery_read_bytes_worst_case,
        recovery_crc_bytes_worst_case=recovery_crc_bytes_worst_case,
    )


def main() -> None:
    cadence = 4.0
    print(
        "devices,pages,region_kib,slots,no_edge_fill_hours,"
        "erases_per_cycling_page_year,total_program_mib_year,"
        "duplicate_scan_kib,recovery_scan_kib"
    )
    for devices in (10, 30, 50, 100):
        for pages in (16, 24, 32):
            r = model(devices, pages, cadence)
            print(
                f"{r.devices},{r.pages},{r.region_bytes / 1024:.0f},"
                f"{r.slots},{r.no_edge_fill_hours:.2f},"
                f"{r.erases_per_cycling_page_year:.2f},"
                f"{r.total_program_bytes_per_year / (1024 * 1024):.2f},"
                f"{r.duplicate_scan_read_bytes / 1024:.1f},"
                f"{r.recovery_read_bytes_worst_case / 1024:.1f}"
            )


if __name__ == "__main__":
    main()
