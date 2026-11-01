"""2. adım: internetsiz harita paketleri.

Sunucu, harita klasöründeki hazır `.pmtiles` dosyalarını uygulamaya verir.
Her paketin yanında `tools/maps/make_offline_catalog.py sidecar` ile yazılmış
`<ad>.pmtiles.json` bulunur (kimlik, sürüm, sınırlar, boyut, SHA-256, atıf).
Sunucu dosyayı her istekte yeniden özetlemez; uygulama indirirken SHA-256'yı
kendisi doğrular. Yanındaki bilgi eksik/bozuk ya da boyut tutmuyorsa paket
katalogda görünmez.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import re

NAME = re.compile(r"[a-z][a-z0-9-]{0,39}-[A-Za-z0-9][A-Za-z0-9._-]{0,39}\.pmtiles")
FILE_ROUTE = "/api/v1/maps/file/"
CATALOG_ROUTE = "/api/v1/maps/catalog"
KINDS = ("protomaps-v4-vector", "licensed-satellite-raster")
FIELDS = ("id", "version", "title", "kind", "bounds", "minZoom", "maxZoom",
          "bytes", "sha256", "attribution", "license")


def default_dir() -> Path:
    base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(base) / "orun" / "maps"


def _entry(meta: object, name: str, size: int) -> dict | None:
    if not isinstance(meta, dict) or any(k not in meta for k in FIELDS):
        return None
    if f"{meta['id']}-{meta['version']}.pmtiles" != name:
        return None
    b = meta["bounds"]
    if (meta["kind"] not in KINDS or not isinstance(meta["bytes"], int) or
            meta["bytes"] != size or
            not isinstance(meta["sha256"], str) or
            not re.fullmatch(r"[0-9a-f]{64}", meta["sha256"]) or
            not isinstance(b, list) or len(b) != 4 or
            not all(isinstance(v, (int, float)) for v in b) or
            not all(isinstance(meta[k], int) for k in ("minZoom", "maxZoom"))):
        return None
    out = {k: meta[k] for k in FIELDS}
    out["url"] = FILE_ROUTE + name
    return out


class MapPacks:
    def __init__(self, directory: str | Path):
        self.dir = Path(directory)

    def _packs(self) -> list[tuple[str, dict]]:
        try:
            names = sorted(p.name for p in self.dir.iterdir())
        except OSError:
            return []
        found = []
        for name in names:
            if not NAME.fullmatch(name):
                continue
            path = self.dir / name
            try:
                if path.is_symlink() or not path.is_file():
                    continue
                meta = json.loads((self.dir / (name + ".json")).read_text("utf-8"))
                entry = _entry(meta, name, path.stat().st_size)
            except (OSError, ValueError):
                continue
            if entry is not None:
                found.append((name, entry))
        return found

    def catalog(self) -> dict:
        return {"schema": "orun-offline-packs-v1",
                "packs": [entry for _, entry in self._packs()][:100]}

    def file(self, name: str) -> Path | None:
        """Yalnız katalogda görünen bir paket dosyası."""
        if not NAME.fullmatch(name):
            return None
        for known, _ in self._packs():
            if known == name:
                return self.dir / name
        return None
