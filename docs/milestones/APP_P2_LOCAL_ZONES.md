# APP P2 — Alanlar: yerel mera taslakları (Android)

## Dayanak ve kapsam

Kaynak prototipler: kullanıcının `ORUN-Saha.html`, `ORUN-Mera (1).html` ve `ORUN-Mera.html` dosyaları.
Bu geliştirme yalnız mevcut `edge/app` React + MapLibre uygulamasındaki boş
`Alanlar` ekranını tamamlar. Mevcut cihaz/geofence firmware koduna, Gateway
protokolüne, teslim/onay zincirine ve sunucu veritabanına dokunmaz.

## Telefon davranışı

- Alanlar sekmesinde mevcut yerel taslaklar, ölçüleri ve dahil etme durumu gösterilir.
- Yeni mera / düzenleme haritası, nişangâhla haritayı kaydırarak veya haritaya dokunarak
  köşe ekler. Köşeler sürüklenebilir; kenar ortasındaki (+) ile yeni köşe eklenir.
  Geri al ve yinele mevcuttur.
- Alanın hektar/dekar, çevre ve köşe sayısı gösterilir.
- E7 (1e-7 derece) koordinat modeli, kapatılmış GeoJSON halkası,
  kesişen kenar/sıfır alan, komşu tekrar, küresel tekillikler, 10 derecelik
  yerel açıklık, en çok 8 **dahil edilen** alan ve toplam 64 köşe kontrol edilir.
- Değişiklikler `localStorage["orun.fences.v1"]` altında sadece **bu telefonun
  WebView uygulama alanına** yazılır; veri silme/uygulama sıfırlama taslakları
  kaybettirebilir. GeoJSON dışa aktarım yedeği kullanılabilir.
- GeoJSON Polygon (WGS84 [lon,lat], tek kapalı halka) içe alınır; MultiPolygon,
  delikler ve yanlış set bütçesi reddedilir. Önce doğrulama, sonra bütün seti yazma.
- Harita karoları çevrimiçi Esri kaynağından gelir. Bağlantı yoksa MapLibre'nin
  arka planı ve yerel çokgenleri yine çizilir; bu çevrimdışı harita paketi değildir.
- Kaydetme, dahil etme, silme, dışa aktarma **tasmadaki aktif sınırı değiştirmez**.
  Otomatik, zamanlayıcılı veya sahte `APPLIED` sonucu yoktur.

## Ertelenen entegrasyon

Yerel taslakları gerçek cihazlara yetkili komutla gönderme/geri dönüş sonucu,
cihaz uygulama durumu, sunucuda ortak alan arşivi, yürüyerek çizme,
KML, geçmiş konumlarla uyarı simülasyonu ve çevrimdışı harita paket yöneticisi
bu PR'ın kapsamı dışındadır. Mevcut `GeofenceMutationOwner` ve yetkili
transport tasarımı sonraki uygulama aşamasının kaynağıdır.

## Doğrulama

Yeni `edge/app/test/zones.test.ts` testleri: E7 çokgen,
çakışma/dejenerasyon, kapasite, GeoJSON round-trip, reddedilen içe aktarım
ve yerel kalıcılık. Kod gönderildikten sonra sahibin makinesinde
`npm test && npm run build` çalıştırılmalıdır. Daha sonra fiziksel Android
harita/drag/drop/file-input denemesi yapılmalıdır.

## Crosshair UX parity with the original ORUN-Saha HTML (2026-10-26)

Phone trial feedback: the first React build's plain "+" glyph and large
fixed bottom form were not equivalent to the reference UI.

- CSS pseudo-element sight is pinned at exactly `left: 50%; top: 50%` of
  the MapLibre container; the saved vertex uses the same `map.getCenter()`.
- Camera `move` updates the live cursor's next-edge and closing-edge dashed
  preview lines, a filled tentative polygon, provisional area/perimeter, and
  next-segment distance. Draft vertices are not committed until pressing +.
- Draw controls are a compact bottom bar with central large green + button;
  undo/redo and input mode flank it. Area/corner/perimeter strip moves to top.
- Mera name and active flag move to the final save sheet; map remains visible.
- Editing an existing area starts in tap mode, matching the original prototype.
- Previewing changes does not write the phone's draft storage or the device.

The visual and touch flows still need a new on-phone smoke test after rebuild;
the earlier 45-test APK predates this UI revision.

## Toplu UX düzeltmesi ve açık kalemler (26 Ekim 2026)

Kullanıcı geri bildirimi: her küçük değişiklikte yeni APK oluşturma; önce
eski Claude prototipini ve ürün kapsamını temel alarak biriktir, sonrasında
**tek saha derlemesi** yap.

Bu dalda biriktirilenler (host testi / Android derlemesi YENİ HEAD'de yapılmadı):

- Harita merkezinde gerçek nişangâh, canlı kesik kenar/alan önizlemesi,
  hareketle güncellenen ölçüler ve metre etiketleri, tam ekran sade çizim çubuğu.
- Yanlışlıkla çıkışta kaydedilmemiş sınırların silinmesi için açık onay.
- Diğer aktif alanları dikkate alan **kalan toplam köşe kapasitesi**; alana yer
  kalmadığında bilgilendirme ve çokgenin kesişmesini engelleyerek en fazla %2
  alan kaybıyla muhafazakâr sadeleştirme (geri al ile iptal).
- Eski `orun.fences.v1` HTML taslakları `active` alanı yoksa `true` olarak
  açılabilir; okunamayan eski JSON'un üzerine yeni veri yazılmaz.
- Ana **Harita** ekranında yerel mera taslakları kesik çizgili gösterilir;
  katman menüsünden gizlenebilir. Stil değişimi ve Alanlar'dan Harita'ya dönüş
  sırasında görünürlük/kayıt yenilenir. Kesik çizgi, **tasmaya uygulanmış
  sınır anlamına gelmez**.
- Önceden başarısız olan otomatik Android debug APK workflow'u bu PR'dan
  kaldırıldı: sahadaki APK üretimi inceleme sonunda bir defa yapılacak.

**İkinci aşama / henüz eksik:**
- Telefon GPS'iyle sınır boyunca yürüyerek kayıt;
- GeoJSON yanında KML;
- Kapsama hücreleriyle düşük bağlantı uyarısı;
- Geçmiş tasma izlerinde 30-gün sanal çit uyarısı simülasyonu;
- Yer işaretleri (su, tuz, kapı, barınak vb.), çizgili dolgu ve gelişmiş
  alan adları/etiketleri;
- Çevrimdışı harita indirme/yönetimi;
- Taslakları gerçek tasmaya **yetkili gönderim**, dayanıklı teslim/uygulanma
  kanıtı ve yetki kontrolü; bu iş `APP P2` yerel taslak kapsamında değildir.

**Sonraki tek doğrulama:**
`cd ~/ORUN-TLP-SYSTEM && git fetch origin feat/app-zones-local-editor`;
`cd ~/ORUN-UPLOAD-VERIFY && git switch --detach origin/feat/app-zones-local-editor`;
`cd edge/app && npm test && npm run build`. Önceki HEAD 45/45 ve
Vite build başarılıydı; bu yeni revizyon için sonuç **henüz bilinmiyor**.
Testler geçtiğinde yalnız bir kez `npx cap sync android` + Gradle APK,
telefonda çizim/kaydetme/harita gösterimi/geri dönüş davranış testi.

## Field map livestock balloon visibility (same deferred APK)

Owner feedback: fixed 46 px normal / 58 px selected balloons hide fine-scale
GNSS positions, and far-zoom symbols run into each other. The map also
stopped clustering at zoom >=17.5, even when two collars report identical
positions.

Changes:
- Separate, pure `src/features/map/pinLayout.ts` with zoom-responsive visual
  diameters: about 30 px at zoom 14, 22 px at zoom 18 and 19 px at zoom 20,
  with selected markers only 4 px larger. At distant zoom they shrink to
  18 px. Actual tappable surface remains 44 px.
- CSS variables update continuously during zoom gestures; no repeated marker
  reconstruction, coordinate snapping, or GNSS change.
- Compact pixel-space collision grouping continues at ALL zoom levels;
  bounded grouping deliberately avoids coalescing an entire row/chain of
  otherwise distinct animals. Count bubbles stay legible and retain the
  highest-priority status colour.
- Selected animal remains at its true fix with a clickable `+N` indicator.
  At close zoom (or identical fixes), a bubble opens a scrollable list of
  animals instead of zooming forever; users may select one from that list.
- The phone's own blue position dot is elevated above animal symbols.
- Names of unselected animals appear only at detailed zoom and when there
  is screen space; the selected animal's label stays visible.
- New pure tests cover zoom sizing, separate fixes, unavoidable overlaps,
  selection, zoom-driven collision and prevention of long-chain coalescing.

**Verification pending:** next combined `npm test && npm run build`, followed
by a single Android APK field test. Must check at zoom 9/14/18/20, with both
distinct and identical collar fixes, and check the phone's blue dot at the
same point as a collar. No APK was built as part of this change.

## Zoomed satellite imagery disappearing: source-level fallback (2026-10-26)

Owner reports that the satellite basemap vanishes when zooming close on Android.
Source inspection: MapLibre camera allows zoom 20; Esri World Imagery raster
source was maxzoom 19. Reaching 20 alone is NOT a cause (the 19-tile should
normally overzoom). But at selected locations and high zoom, absent/failed
raster tiles have no lower-resolution imagery to display. There was only
one imagery layer in the main map and in the Alanlar editor.

Fix now staged in this PR (not yet validated on-device):

- Shared `features/map/satelliteStyle.ts` makes a two-layer Esri raster style.
  A z15-limited underlay is always visible and is enlarged by MapLibre.
  A separate z19 detail raster sits over it where detailed tiles exist.
  When a high detail tile is absent, its transparent/empty tile space leaves
  the lower-resolution underlay visible. Same style is used by both screens.
- `cancelPendingTileRequestsWhileZooming: false` in both map constructors
  reduces transient blank frames during pinch zoom while newly required
  imagery tiles download.
- Camera maxZoom 20 stays unchanged; do not deprive users of close-up
  polygon editing. The underlying geofence/GNSS locations are unaffected.
- Separate host tests assert layer order, source zoom caps and same tile source
  for main-map and editor.
- This is **not offline satellite caching**. Both layers still need internet;
  server-side failures across all levels, WebGL canvas failures, or blocked
  ArcGIS network access require further diagnosis. User's exact failure reason
  remains unproven without phone capture/logcat at the failing zoom.

Future combined APK smoke test: compare z14/16/18/19/20, pan in multiple
areas, check while mobile Internet is working, and check no imagery regression
when map style changes or polygons are being edited.

## Owner field screenshot / issue #181 / staged safe-mode workaround

After APK `4d16d24` was installed the Map screen displayed a grid of
**grey tiles actually containing the words "Map data not yet available"**.
The owner's livestock icon and coverage squares remained visible. This proves
the problem is imagery data, not GNSS or marker rendering. Esri documents that
tile servers can return a placeholder **image** instead of 404; the z19 raster
therefore obscures the underlying z15 layer added earlier.

New, not yet built in the Android APK:
- `satelliteRasterParts("base", true)` uses only a z15-capped source, skipping
  the z19 opaque placeholder layer completely; MapLibre scales the real z15
  image for camera zoom 16–20. It deliberately does **not** invent finer data
  and may still fail if source imagery is unavailable already at z15.
- In Harita > Katmanlar, "Güvenli uydu görüntüsü" allows user selection,
  persists with other preferences and uses the same setting in Mera editor.
  Default remains high-detail mode, retaining full Esri detail where present.
- Host test checks the layer/source tree for both regular/secure modes.

This is **not** a replacement for legal offline downloads. Separate issue
#181 scopes package selection (e.g., Aydın/Muğla low-zoom PMTiles), user-drawn
high-resolution pasture subregions with explicit size estimate, downloaded
package verification/resumption, provider offline license and persistent
on-device tile storage. Avoid unlicensed bulk download from Esri/OSM/Google.

Status: GitHub staged, not run on a physical device and not included in the
already installed APK. Test/app build gate before any future deployment.
