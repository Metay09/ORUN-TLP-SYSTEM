# ORUN — Çevrimdışı bölgesel harita paketleri (M1)

Bu aşamada **gerçek çevrimdışı harita henüz açılmıyor**. Önce boyutu sınırlanmış,
lisansı izin veren, SHA-256 ile doğrulanan hazır `.pmtiles` paketlerinin
telefona aktarılmasının güvenli altyapısı geliştiriliyor.

## Kaynaklar ve haklar

- Protomaps Basemap v4 (OSM türevi; ODbL Produced Work, OSM atfı gerekir):
  https://docs.protomaps.com/basemaps/downloads
- PMTiles CLI: https://docs.protomaps.com/pmtiles/cli
- Açık OSMF tile sunucularından önceden toplu indirme **yasaktır**:
  https://operations.osmfoundation.org/policies/tiles/
- Google Maps veya Esri görüntülerini sağlayıcı izni olmadan arşivleme.
  Uydu görüntüsü için **çevrimdışı yeniden dağıtım izni** olan ayrı veri
  kaynağı ve maliyet modeli gereklidir.

## Hedef paket ayrımı

1. **Aydın temel harita:** OpenStreetMap / Protomaps tabanlı vektör PMTiles,
   z0–15. Şehir, köy, yol, su, arazi altyapısını taşır; üst zoomda sadece
   büyütülür. İl sınırı değil, üretim sırasında seçilmiş bölge sınırıdır.
2. **Muğla temel harita:** Aynı kaynak ve format, z0–15.
3. **Mera uydu görüntüleri:** Ayrı, seçilebilir ve lisansı kanıtlanmış raster
   PMTiles; gerçek çözünürlüğün yettiği en üst seviye belirtilir.

*Bu depoda henüz Aydın veya Muğla PMTiles arşivi yok.* Güncel Protomaps
build sayfasından uygun sürüm seçilmeli; sabit bir dünya kaynak URL'sinin
sonsuz kullanılabilir olduğu varsayılmamalıdır.

## Paket hazırlama (Debian 12, hazırlık yapan yetkili bilgisayar)

PMTiles CLI kurulduktan sonra Protomaps'in uygun lisanslı **hazır basemap**
dosyası veya sahibin aynı formatta hazırladığı yerel kaynak üzerinden
bölge çıkarılır. Aşağıdaki sınırlar **yalnız örnek kaba dikdörtgendir**,
resmî il sınırı değildir; gerçek dağıtımda bölge GeoJSON poligonları tercih
edilir:

```bash
mkdir -p ~/orun-harita-paketleri
pmtiles extract /PATH/TO/VERIFIED-SOURCE.pmtiles \
  ~/orun-harita-paketleri/aydin.pmtiles \
  --bbox=27.0,37.0,29.1,38.6 --maxzoom=15
pmtiles extract /PATH/TO/VERIFIED-SOURCE.pmtiles \
  ~/orun-harita-paketleri/mugla.pmtiles \
  --bbox=27.0,36.1,30.0,37.8 --maxzoom=15

pmtiles verify ~/orun-harita-paketleri/aydin.pmtiles
pmtiles verify ~/orun-harita-paketleri/mugla.pmtiles
```

**Önemli:** Bunlar çalışma yönergesidir; harita kaynağının mevcut ve
doğrulanmış olduğu iddiası değildir. Paket dosyaları oluşturulana kadar
telefon uygulamasına çalışmayan indirme düğmesi konulmayacak.

Dağıtım sunucusunda **HTTPS + CORS** ve büyük arşivlerde güvenilir aralık
istekleri (HTTP Range) sağlanmalı. Gelecek katalog üretim örneği:

```bash
python3 tools/maps/make_offline_catalog.py \
  ~/orun-harita-paketleri/aydin.pmtiles \
  --id aydin-roads --version 2026-10-26 --title 'Aydın temel harita' \
  --kind protomaps-v4-vector --bounds 27.0,37.0,29.1,38.6 \
  --minzoom 0 --maxzoom 15 \
  --url https://YOUR-AUTHORIZED-MAP-HOST.example/aydin.pmtiles \
  --attribution '© OpenStreetMap contributors · Protomaps' \
  --license 'ODbL Produced Work' \
  --output ~/orun-harita-paketleri/catalog.json
```

**URL örnektir ve çalışacağı iddia edilmez.** Katalog uygulamaya gömülecek
veya sahibin HTTPS sunucusundan doğrulanmış olarak sunulacak; telefon hiçbir
zaman rastgele üçüncü taraf URL'lerinden uydu karosu toplamayacak.

## Android M1 teknik sınırları

Kod: `edge/app/src/features/map/offlinePacks/catalog.ts` ve `store.ts`.

- Bilinen kaynak/format/sürüm/boyut/URL/bbox/SHA-256 doğrulaması, z15
  vektör sınırı, z20 üst kamera ayrı tutulur.
- Paket çekimi **tek dosya**, `fetch` stream + OPFS + noble SHA-256.
  Çok büyük dosya bir kerede JS belleğine alınmaz.
- `installed.json` yalnız indirme, hash ve header doğrulamasından sonra
  yazılır. Yarım indirme hiçbir zaman "kuruldu" olmaz.
- Harita arşivi, tasmadan toplanan SQLite kayıtlarından ayrı OPFS altında
  tutulur. Uygulama güncellemesinde korunması beklenir; **uygulamayı kaldırma,
  verileri temizleme, depolama kotası ve WebView davranışı** ayrıca ele alınmalı.
- Depolama tahmini yaklaşık bir kotadır; Android gerçek cihazda disk
  kontrolü ve yetersiz alan testleri şart.
- **Henüz:** kesintiden devam etme, paket indirme UI, otomatik lisans
  yetkilendirmesi, PMTiles dosyasını MapLibre haritasına bağlama, yerel
  etiket/font paketi, vektör stil, sunucu manifest servisi, uydu kaynak
  anlaşması **bu M1'de yoktur**. Bunlar tamamlanana dek fonksiyon
  kullanıcıya "indirildi, açıldı" diye sunulmayacak.

## Ana harita entegrasyonu (M2)

`pmtiles` JS kütüphanesi + MapLibre GL JS `addProtocol` kullanılarak
OPFS dosyasından `FileSource` ile PMTiles okunacak. Bölge PMTiles
vector v4 ile uyumlu yerel stiller (roads/places/water/landuse) ve
**çevrimdışı yer adları için yerel glyph/sprite paketleri** hazırlanacak.
Harita tabanı değişse dahi hayvan, iz, kapsama, GPS ve mera katmanlarının
yaşam döngüsü ayrı kalacak. Paketin dışında "indirilmeyen bölge" mesajı
görünecek.

Ayrı offline katalog ve kurulum testleri:

```bash
cd edge/app
npm test
npm run build
cd ../..
python3 -m unittest tools/maps/test_make_offline_catalog.py
```

Bu komutlar **Android APK oluşturmaz**. Fiziksel Samsung/Capacitor OPFS
denemesi ve M2 entegrasyonu ayrıca yapılacak.
