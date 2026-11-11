#!/usr/bin/env python3
"""ORUN telefon uygulaması indirme sayfası.

  python3 tools/orun_apk_sayfa.py ekle <klasör> <apk> <sha> <başlık>
  python3 tools/orun_apk_sayfa.py sayfa <klasör>

Klasörde yalnız ORUN-<sha>.apk ve yanında ORUN-<sha>.json (tarih, başlık)
durur; en yeni 5 sürüm kalır, eskiler silinir. index.html: üstte büyük
"En yeni" kartı ve indirme düğmesi, altında eski sürümler (tarihleriyle).
"""
from __future__ import annotations

import html
import json
import shutil
import sys
import time
from pathlib import Path

KEEP = 5
AYLAR = ["Oca", "Şub", "Mar", "Nis", "May", "Haz", "Tem", "Ağu", "Eyl", "Eki", "Kas", "Ara"]


def releases(folder: Path) -> list[dict]:
    out = []
    for meta in folder.glob("ORUN-*.json"):
        try:
            m = json.loads(meta.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        apk = meta.with_suffix(".apk")
        if apk.is_file():
            m["file"] = apk.name
            m["size"] = apk.stat().st_size
            out.append(m)
    out.sort(key=lambda m: m.get("at", 0), reverse=True)
    return out


def when(at: float) -> str:
    t = time.localtime(at)
    today = time.localtime()
    day = "Bugün" if (t.tm_year, t.tm_yday) == (today.tm_year, today.tm_yday) else \
        f"{t.tm_mday} {AYLAR[t.tm_mon - 1]} {t.tm_year}"
    return f"{day} {t.tm_hour:02d}:{t.tm_min:02d}"


def mb(size: int) -> str:
    return f"{size / 1_000_000:.0f} MB"


def page(rel: list[dict]) -> str:
    if not rel:
        body = '<p class="empty">Henüz sürüm yok.</p>'
    else:
        new, old = rel[0], rel[1:]
        e = html.escape
        body = f"""
<section class="new">
  <div class="tag">EN YENİ</div>
  <div class="ver">ORUN {e(new["sha"])}</div>
  <div class="when">{e(when(new["at"]))} · {mb(new["size"])}</div>
  <div class="what">{e(new.get("title", ""))}</div>
  <a class="get" href="{e(new["file"])}" download>İndir ve kur</a>
</section>"""
        if old:
            rows = "\n".join(
                f'<li><a href="{e(m["file"])}" download><span class="v">{e(m["sha"])}</span>'
                f'<span class="w">{e(when(m["at"]))}</span>'
                f'<span class="t">{e(m.get("title", ""))}</span></a></li>'
                for m in old)
            body += f'\n<h2>Eski sürümler</h2>\n<p class="hint">Yalnız geri dönmek gerekirse.</p>\n<ul class="old">\n{rows}\n</ul>'
    return f"""<!doctype html>
<html lang="tr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ORUN uygulaması</title>
<style>
body{{margin:0;font-family:system-ui,sans-serif;background:#f4f2ec;color:#1d2a22}}
main{{max-width:520px;margin:0 auto;padding:24px 18px 48px}}
h1{{font-size:22px;margin:0 0 18px}}
.new{{background:#1f5135;color:#fff;border-radius:18px;padding:22px;box-shadow:0 6px 20px #0002}}
.tag{{font-size:12px;letter-spacing:.12em;opacity:.8}}
.ver{{font-size:28px;font-weight:800;margin:6px 0 2px}}
.when{{opacity:.85}}
.what{{margin:12px 0 18px;line-height:1.4}}
.get{{display:block;text-align:center;background:#f3c64b;color:#1d2a22;font-weight:800;
  font-size:19px;padding:16px;border-radius:12px;text-decoration:none}}
h2{{font-size:15px;margin:30px 0 2px;color:#5a645d}}
.hint{{margin:0 0 10px;color:#7a837c;font-size:13px}}
.old{{list-style:none;padding:0;margin:0}}
.old a{{display:grid;grid-template-columns:auto 1fr;gap:2px 12px;padding:12px 14px;margin-bottom:8px;
  background:#fff;border-radius:12px;color:#5a645d;text-decoration:none}}
.old .v{{font-weight:700}} .old .w{{text-align:right;font-size:13px}}
.old .t{{grid-column:1/3;font-size:13px;color:#7a837c}}
.empty{{color:#7a837c}}
</style></head><body><main>
<h1>ORUN uygulaması</h1>{body}
</main></body></html>
"""


def add(folder: Path, apk: Path, sha: str, title: str) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    # Eski düzenden kalan karışık dosyalar (app-debug.apk, metadata) burada olmaz.
    for stray in folder.iterdir():
        if stray.is_file() and not stray.name.startswith("ORUN-") and stray.name != "index.html":
            stray.unlink()
    shutil.copyfile(apk, folder / f"ORUN-{sha}.apk")
    (folder / f"ORUN-{sha}.json").write_text(
        json.dumps({"sha": sha, "title": title, "at": time.time()}, ensure_ascii=False), encoding="utf-8")
    for old in releases(folder)[KEEP:]:
        (folder / old["file"]).unlink(missing_ok=True)
        (folder / old["file"]).with_suffix(".json").unlink(missing_ok=True)
    # JSON'u olmayan eski APK'lar (önceki düzen) da gider.
    for apk_file in folder.glob("ORUN-*.apk"):
        if not apk_file.with_suffix(".json").exists():
            apk_file.unlink()


def main(argv: list[str]) -> int:
    if len(argv) >= 6 and argv[1] == "ekle":
        folder = Path(argv[2])
        add(folder, Path(argv[3]), argv[4], argv[5])
    elif len(argv) == 3 and argv[1] == "sayfa":
        folder = Path(argv[2])
    else:
        print(__doc__, file=sys.stderr)
        return 2
    (folder / "index.html").write_text(page(releases(folder)), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
