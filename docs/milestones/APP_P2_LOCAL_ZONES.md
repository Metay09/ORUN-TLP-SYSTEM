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
