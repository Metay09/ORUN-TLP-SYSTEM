#!/usr/bin/env python3
"""Create a deterministic ORUN catalog entry for *prebuilt, authorized* PMTiles.

This program hashes local archives. It never downloads or scrapes public tiles.
Before publishing, use 'pmtiles verify yourfile.pmtiles' separately.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from urllib.parse import urlparse
import re


def make_entry(
    path: Path, *, pack_id: str, version: str, title: str,
    kind: str, bounds: list[float], min_zoom: int, max_zoom: int,
    url: str, attribution: str, license_name: str,
) -> dict:
    if not re.fullmatch(r"[a-z][a-z0-9-]{0,39}", pack_id):
        raise ValueError("Invalid pack id")
    if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9._-]{0,39}", version):
        raise ValueError("Invalid version")
    if not title.strip() or len(title) > 90:
        raise ValueError("Invalid title")
    if kind not in ("protomaps-v4-vector", "licensed-satellite-raster"):
        raise ValueError("Unsupported kind")
    if len(bounds) != 4 or not (
        -180 <= bounds[0] < bounds[2] <= 180 and
        -85.051129 <= bounds[1] < bounds[3] <= 85.051129
    ):
        raise ValueError("Invalid bounds")
    if not (0 <= min_zoom <= max_zoom <= 20):
        raise ValueError("Invalid zoom range")
    if kind == "protomaps-v4-vector" and max_zoom > 15:
        raise ValueError("Protomaps v4 supports max zoom 15 in ORUN")
    addr = urlparse(url)
    if (
        addr.scheme != "https" or not addr.netloc or
        addr.username or addr.password or addr.fragment or
        not addr.path.endswith(".pmtiles")
    ):
        raise ValueError("An HTTPS .pmtiles package URL is required")
    if not attribution.strip() or not license_name.strip():
        raise ValueError("Attribution and license required")

    with path.open("rb") as handle:
        if handle.read(8) != b"PMTiles\x03":
            raise ValueError("Not a PMTiles v3 archive")
        handle.seek(0, 2)
        size = handle.tell()
        if not 127 <= size <= 2 * 1024**3:
            raise ValueError("Package must be between 127 bytes and 2GiB")
        handle.seek(0)
        digest = hashlib.sha256()
        while block := handle.read(1024 * 1024):
            digest.update(block)
    return {
        "id": pack_id, "version": version, "title": title.strip(), "kind": kind,
        "bounds": bounds, "minZoom": min_zoom, "maxZoom": max_zoom,
        "bytes": size, "sha256": digest.hexdigest(),
        "url": url, "attribution": attribution.strip(), "license": license_name.strip(),
    }


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("file", type=Path, help="Prebuilt and legally redistributable .pmtiles")
    p.add_argument("--id", required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--title", required=True)
    p.add_argument("--kind", choices=["protomaps-v4-vector", "licensed-satellite-raster"], required=True)
    p.add_argument("--bounds", required=True, help="west,south,east,north in WGS84; not exact province border")
    p.add_argument("--minzoom", type=int, required=True)
    p.add_argument("--maxzoom", type=int, required=True)
    p.add_argument("--url", required=True, help="Owner-controlled HTTPS download URL")
    p.add_argument("--attribution", required=True)
    p.add_argument("--license", required=True)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    try:
        bounds = [float(v) for v in args.bounds.split(",")]
        entry = make_entry(
            args.file, pack_id=args.id, version=args.version,
            title=args.title, kind=args.kind, bounds=bounds,
            min_zoom=args.minzoom, max_zoom=args.maxzoom, url=args.url,
            attribution=args.attribution, license_name=args.license,
        )
        if args.output.exists():
            catalog = json.loads(args.output.read_text("utf-8"))
            if catalog.get("schema") != "orun-offline-packs-v1" or not isinstance(catalog.get("packs"), list):
                raise ValueError("Refusing to overwrite unknown or invalid catalog")
        else:
            catalog = {"schema": "orun-offline-packs-v1", "packs": []}
        catalog["packs"] = [
            v for v in catalog["packs"]
            if not (v.get("id") == entry["id"] and v.get("version") == entry["version"])
        ] + [entry]
        if len(catalog["packs"]) > 100:
            raise ValueError("Catalog exceeds 100 packages")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        tmp = args.output.with_suffix(args.output.suffix + ".tmp")
        tmp.write_text(json.dumps(catalog, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        tmp.replace(args.output)
        print(f"CATALOG OK: {args.output} pack={entry['id']} bytes={entry['bytes']} sha256={entry['sha256']}")
    except (ValueError, OSError, KeyError, TypeError) as exc:
        p.error(str(exc))


if __name__ == "__main__":
    main()
