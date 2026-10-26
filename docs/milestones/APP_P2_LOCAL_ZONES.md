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
