#!/usr/bin/env python3
"""Deterministic SF4B gateway-custody capacity/wear planning model.

This is not a partition selector and contains no flash-endurance assumption.
It models the current SF4B candidate geometry under a specified retained-record
production cadence, assuming every produced object is committed by one gateway.
"""

from dataclasses import dataclass

PAGE_BYTES = 4096
RECORD_BYTES = 88
PAGE_HEADER_BYTES = 64
RECORDS_PER_PAGE = (PAGE_BYTES - PAGE_HEADER_BYTES) // RECORD_BYTES
HOURS_PER_YEAR = 24 * 365


@dataclass(frozen=True)
class Result:
    devices: int
    pages: int
    observations_per_device_hour: float
    region_bytes: int
    slots: int
    no_edge_fill_hours: float
    erases_per_page_year: float


def model(devices: int, pages: int, observations_per_device_hour: float) -> Result:
    if devices <= 0 or pages < 2 or observations_per_device_hour <= 0:
        raise ValueError("devices>0, pages>=2 and cadence>0 are required")
    slots = pages * RECORDS_PER_PAGE
    aggregate_per_hour = devices * observations_per_device_hour
    records_per_year = aggregate_per_hour * HOURS_PER_YEAR
    return Result(
        devices=devices,
        pages=pages,
        observations_per_device_hour=observations_per_device_hour,
        region_bytes=pages * PAGE_BYTES,
        slots=slots,
        no_edge_fill_hours=slots / aggregate_per_hour,
        erases_per_page_year=records_per_year / slots,
    )


def main() -> None:
    cadence = 4.0  # 15-minute retained observation cadence.
    print("devices,pages,region_kib,slots,no_edge_fill_hours,erases_per_page_year")
    for devices in (10, 30, 50, 100):
        for pages in (16, 24, 32):
            r = model(devices, pages, cadence)
            print(
                f"{r.devices},{r.pages},{r.region_bytes / 1024:.0f},"
                f"{r.slots},{r.no_edge_fill_hours:.2f},"
                f"{r.erases_per_page_year:.2f}"
            )


if __name__ == "__main__":
    main()
