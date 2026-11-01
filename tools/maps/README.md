# ORUN — İnternetsiz harita (2. adım)

## Nasıl çalışır

1. **PC (sunucu):** `bash tools/orun_harita.sh`
   - Protomaps'in güncel OpenStreetMap haritasından yalnız Aydın–Muğla
     dikdörtgenini (`27.0,36.3,29.9,38.3`, z0–15) indirir (`pmtiles extract`,
     go-pmtiles v1.31.2, SHA-256 sabit).
   - `pmtiles verify` ile doğrular, `~/.local/share/orun/maps/` altına
     `aydin-mugla-<YYYYMMDD>.pmtiles` + yanında `.json` bilgi dosyası yazar
     (`make_offline_catalog.py --sidecar`). Eski sürüm, yenisi hazır olunca
     silinir. Paket güncelse hiçbir şey indirmez.
   - `tools/orun_guncelle.sh` paket hiç yoksa bunu kendisi çalıştırır.
2. **Sunucu:** `orun_sunucu.py` paketleri hesap oturumuyla verir:
   `GET /api/v1/maps/catalog`, `GET /api/v1/maps/file/<ad>.pmtiles`
   (`--maps-dir`, varsayılan `~/.local/share/orun/maps`). Yalnız yanında
   doğru bilgi dosyası olan ve boyutu tutan paketler listelenir.
3. **Telefon:** Ayarlar → İnternetsiz harita → İndir. Dosya akış halinde
   telefonun uygulama deposuna (OPFS) iner, SHA-256 ve PMTiles başlığı
   doğrulanmadan "kuruldu" sayılmaz. Yeni sürüm varsa "Yeni sürümü indir".
4. **Harita:** paket telefondaysa
   - sokak haritası her zaman bu paketi kullanır (internet gerekmez),
   - uydu/arazi seçiliyken internet yoksa (telefon çevrimdışı ya da
     görüntüler art arda gelmiyorsa) bu harita kendiliğinden gösterilir;
     internet dönünce uydu geri gelir (30 sn'de bir yoklanır).
   Yazı tipi (Noto Sans) ve simgeler uygulamanın içindedir
   (`edge/app/public/map-assets`), internetten istenmez.

## Haklar

- Harita verisi: © OpenStreetMap katkıcıları, Protomaps Basemap v4
  (ODbL Produced Work; atıf haritanın (i) düğmesinde).
- Protomaps sürümleri yalnız `pmtiles extract` ile bir kez çekilip kendi
  sunucumuzda tutulur (Protomaps'in önerdiği kullanım).
- **Uydu görüntüsü internetsiz yok (2b):** Esri World Imagery / Wayback
  çevrimdışı kullanıma yalnız ArcGIS uygulamaları içinde izin veriyor;
  Google/Bing de izin vermiyor. Lisanslı bir kaynak seçilince
  `licensed-satellite-raster` türüyle aynı yoldan eklenecek.

## Testler

```bash
python3 -m pytest -q backend/tests/test_internetsiz_harita.py tools/maps/test_make_offline_catalog.py
cd edge/app && node --test --experimental-strip-types test/internetsizHarita.test.ts test/offlinePacks.test.ts
```
