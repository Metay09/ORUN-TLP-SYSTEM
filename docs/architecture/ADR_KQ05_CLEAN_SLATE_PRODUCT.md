# ADR — ORUN tek-zincir ürün mimarisi: temiz başlangıç

**Karar:** Kabul edildi (17.11.2026). **Uygulama:** Devam ediyor; bu belge ürünün hazır olduğu anlamına gelmez.
**Kapsam:** RAK4630 Tracker + Gateway + PC Edge + backend + Android.
**Çalışma dalı:** `design/kq05-live-history-receipt-host`, PR #316.

## 1. Kullanıcı/ürün kararı

ORUN henüz gerçek sürü sahasına çıkmadı. Bugüne kadarki test/deneme kayıtlarını, eski Flash sayfalarını,
eski veritabanı satırlarını veya eski firmware/protokol biçimlerini **koruma zorunluluğu yoktur**.
Ürün mimarisinde eski sürümle uyum, veri taşıma, çift yazma, ikinci gönderim/ACK hattı ve
"önceki kayıtlar zarar görmesin" gerekçesiyle yeni özellik geciktirilmeyecektir.

Bu karar **kayıt güvenliğini ortadan kaldırmaz**: nihai sürüm üretildikten sonraki yeni kayıtların
güç kesintisinde, tekrarlı pakette, RF kesintisinde veya pil bitmesinde doğru kalması şarttır.
Test verisi silinebilir; ürün verisi izinsiz kaybolamaz.

## 2. Tek nihai zincir

```text
Tracker GNSS + olaylar + pil/aktivite
  -> TEK ObservationStore / ProductRecordStore (PERIODIC ve EVENT)
  -> şifreli PRODUCT_SECURE (önce son konum, fırsatta geçmiş; RF bütçesi uygulanır)
  -> Gateway tek yetkili durable custody/store&forward
  -> PC Edge tek imzalı, dayanıklı teslim kuyruğu
  -> Backend tek doğrulanmış, kalıcı idempotent kayıt/outbox kaynağı
  -> HMAC'li seçici TOPLU teslim makbuzu
  -> Tracker tam kimlik/scope doğrulaması + Flash'ta yalnız doğrulanan kayıtları serbest bırakma
  -> Android: Canlı/Geçmiş/Harita/Olaylar/Cihazlar/Bağlantı/Ayarlar
```

Bir GNSS ölçümü için **tek kayıt kimliği ve tek kalıcı append**; bir olay için ayrı EVENT
kimliği/append. Eski TLP v1 POSITION ile ProductSecure çift yayını yok.
Konum telemetrisi için **her paket başına ACK bekleme zorunluluğu yok**.
Hizmet/ayar/yetki komutları ve gerçek custody işlemleri için gereken doğrulamalar ayrı ve güçlü kalır.
Uygulama "gönderildi" ile "backend kalıcı teslim aldı" durumunu aynı şey saymaz.

## 3. Kaldırılacaklar — geçiş uyumu yok

| Eski veya geçici yol | Nihai karar | Kaldırma ön koşulu |
| --- | --- | --- |
| Tracker `HistoryStore` ikinci konum yazıcısı | Sil | Tek PERIODIC + EVENT Flash yolu PASS |
| Legacy `POSITION` ve `HISTORY_SECURE` ürün radyo yolları | Sil | ProductSecure gerçek cihazda uçtan uca PASS |
| Legacy `HistoryStoreForwardRuntime`, replay ve eski receipt/bridge dönüşümleri | Sil | Yeni Edge/Backend/Android geçmiş ve canlı zinciri PASS |
| Tracker `TrackerProductTxPilot`, 4 exact-cache slotuna bağımlı canlı gönderim | Sil | Yeni canlı/retained gönderim ile RF ve fail-closed PASS |
| Geriye uyumluluk, eski Flash formatı migration/backup kodu | Yeni ürün imajına alma | Yeni temiz başlangıç ve reset/provision mekanizması PASS |
| Kamuya açık test anahtarları ve BENCH yayını | Ürün imajından çıkar | Gerçek cihaz anahtar tahsisi ve servis tanılama eşdeğeri PASS |
| İşlevsel olarak yinelenen ACK/pacer/replay sayaçları | Tekleştir | Makbuz kimlik/doğrulama modeli PASS |

Bu bir **hemen körlemesine sil** talimatı değildir: eski bir yol nihai özellik zincirinin
tek çalışan halkasıysa önce yeni halkayı tamamla, sonra tüm projeden çıkar. Koruma gerekçesi
eski veri/uyumluluk değil; yeni ürünün fiilen işlevsiz kalmasını önlemektir.

## 4. Mutlaka tamamlanacak ürün özellikleri

1. **Olaylar:** onaylanmış alan dışı geçişi, kayıp, pil ve sistem olayları; ayrı ve kalıcı
   EVENT; olay konumu/zamanı; uygulamada olay geçmişi, önem ve bildirim.
2. **Tek kayıt sahibi:** Tracker'da PERIODIC ve EVENT append'lerinin sahibi ProductRecordStore.
   Yanlış/eksik rol, tam Flash, reset, sıra çakışması ve yazma hatasında çift yol/fallback yok.
3. **Radyo:** canlı konum geçmişi bloke etmez; geçmiş yalnız RF hakkı olduğunda gönderilir;
   gateway doğrulanmış custody olmadan cihaz kayıtlarını bıraktırmaz; bölgesel yayın bütçesi şart.
4. **Toplu makbuz:** gerçek backend kalıcı kabulüne bağlı, scoped HMAC kimlikli,
   seçici/aralıklı, reboot sonrası tutarlı; bir eksik sıra diğerlerini yanlışlıkla serbest bırakmaz.
5. **Gateway/Edge:** gerçek Gateway BLE ingress -> dayanıklı staged kayıt -> gerektiğinde LoRa
   makbuz iletimi; malformed/foreign key/unknown epoch/kapasite dolu durumda açık ret.
6. **Android:** yeni olay ve konum veri modeliyle tutarlı Canlı/Geçmiş; gecikme/tazelik/kayıp
   görünümü; gereksiz çift kayıt ve çift bildirim yok; onaylı diğer ekranlar korunur.
7. **Güç:** gerçek deep sleep/akım ölçümü, GNSS/LoRa/BLE ve pil ömrü doğrulaması.
8. **Saha:** tek bir imzalı firmware zinciri, cihaz başına anahtarlar, RF ölçümü,
   Flash/güç kesintisi testleri ve gözetimli pilot kanıtı.

## 5. Temiz başlangıç / sıfırlama

Eski kayıtları yedekleyip dönüştürmek gerekmiyor. Final imajın *format sürümü*
ve doğrulanmış rol/provisioning kapısı bulunacak. Uyumsuz eski imaj/Flash
üzerinden sessiz veri okuma veya migration/fallback **olmayacak**.
Laboratuvarda yeni ürün imajına geçerken cihazlar **açıkça yetkilendirilmiş**
sıfırlama/yeniden kayıt prosedürüyle temizlenip benzersiz kimliklerle yeniden provision edilecek.
Backend/Edge için de test tabloları/kuşakları kontrollü ve atomik biçimde sıfırlanacak.

Bu ADR silme yetkisiyle çalışan bir komut değildir; PR birleştirmesi, fiziki cihaza
yükleme veya canlı servis/veritabanı silme işlemi başlatmaz.

## 6. Mühendislik sırası ve çıkış kapıları

**P0 — Veri modelini tamamla:** EVENT ekle, geofence append'i doğrula, mevcut PERIODIC
sözleşmesini kırma; gerçek Flash reboot/power-cut host testleri.

**P1 — Gerçek teslim zinciri:** Gateway offer/STAGED + Edge + Backend toplu receipt ->
Tracker release; test/mock yerine gerçek donanımın GATT/LoRa/Flash yoluna bağla.

**P2 — Eski yolları sök:** HistoryStore, POSITION/HISTORY_SECURE, tekli telemetri ACK,
eski pilot ve bench-only yapılandırmaları temiz ürün imajından çıkar.
Normal/legacy hedefin aynı boyutta kalması artık nihai ürün kabul koşulu değildir.
Yeni ürün hedefi daha az kod, daha az iş sahibi ve daha kolay tanılama için optimize edilir.

**P3 — Uygulama ve ürün kalite kapısı:** yeni kimlik/event/receipt modelini Android,
backend ve Edge'de eşleştir; 14 günlük tek tasma pilotuna kadar güvenlik, akım,
radyo ve reboot verilerini ölç.

**PASS ölçütü:** En az bir uçtan uca kayıt/olay/delivery/receipt akışı, tek append sahibi,
gerçek cihazda reboot ve kötü durum testleri, radyo bütçesi ve uygulama görünürlüğü;
tam PC regresyonu ve linker/ELF kanıtı. Bu koşullar sağlanmadan "sahaya hazır" yok.

## 7. Geçerli inceleme sonuçları

17.11.2026'da operatörün doğruladığı `3336baab...`:
- Normal firmware Flash: 463,292 bayt.
- KQ05 LAB Flash: 468,548 bayt (öncekinden **1,320 bayt daha az**).
- Tam PC regresyon ve her iki hedef derlemesi PASS; normal pilot KQ05 ELF'ten çıktı.

**Henüz çözülmemiş:** KQ05 EVENT kalıcı yazıcısı; gerçek Gateway KQ05 Edge offer
durable staging bağlama; tam gerçek RF/BLE kapama; pil uyku ölçümü. Eski test kayıtlarını
koruma bu eksiklerin çözümü için gerekçe değildir.

**Karar ilkesi:** Sağlam çalışan nihai zinciri koru, eski veriyi/geri uyumluluğu koruma.
