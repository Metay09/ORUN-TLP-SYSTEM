# ADR KQ-05 — LIVE / HISTORY / CONTROL ve toplu teslim makbuzu

**Durum:** TASLAK, 14 Kasım 2026. İzleme: #315. Bu belge PC modeli ve geçiş şartlarıdır; yeni üretim RF protokolü veya donanım onayı değildir.

## Kesin kararlar

1. **LIVE:** Konum ve normal telemetri önce kalıcı Flash'a kaydedilir, sonra tekil ACK beklenmeden gönderilir. Bir kayıt teslim edilmedi diye sonraki konum gönderimi engellenmez.
2. **HISTORY:** Geçmiş kayıtlar kalıcı saklanır. Yeni LIVE önceliklidir; HISTORY kalan RF kapasitesinden adil pay alır. TX başlatılması teslim garantisi değildir.
3. **RECEIPT:** Yalnızca yetkili tarafın gerçekten kalıcı sakladığı açık kayıt kimlikleri için kriptografik, tekrar korumalı toplu makbuz. İlk hedef yaklaşık 5 dakika, *sabit beş paket değil*. Eksik kimlik onaylı sayılmaz. 60 dakika ve üzeri trafikte tekil onay mümkün, şart değil.
4. **CONTROL:** Ayarlar, alan/geofence, güvenlik ve firmware işlemleri ayrı imzalı komut ve uygulanmış işlem sonucu kullanır. Radyo gönderimi ile işlemin uygulanması karıştırılmaz.
5. **RF bütçesi:** Taslak #305 içindeki radyo bütçesi LIVE, HISTORY, RECEIPT, CONTROL fiziksel TX'lerinin tamamında geçerlidir. Kota bitince sınırsız tekrar yok.

## Mevcut koddan yeniden kullanılacaklar

- PositionFlow (firmware/src/position_flow.cpp) zaten HistoryStore append başarılı olduktan sonra legacy POSITION yayını yapıyor. Bu yayın tekil ACK beklemiyor. **Ancak legacy POSITION ürün düzeyinde korumalı telemetriyle eşdeğer değildir**; güvenli LIVE protokolü ayrıca kabul edilecek.
- HistoryDeliveryCoordinator, HistoryReceiptAdmissionCoordinator ve HistorySecureCrypto mevcut **BACKEND_DURABLE** toplu makbuz / seçmeli kimlik / replay altyapısıdır. Bu, Gateway'in flash'ına teslim onayı ile **aynı yetki değildir**. Gateway-durable kullanılacaksa ayrı kriptografik kapsam ve yeniden başlatma modeli gerekir.
- Mevcut tlp_v2_history_secure.h en çok 6 açık record identity taşır. Boyut: 36 bayt header + 4 bayt sabit + 8 bayt/kimlik + 8 bayt tag. Beş kayıt **88 bayt**, SF11 ile ~1971ms RF. Bu yalnız mevcut BACKEND_DURABLE taşıyıcı için geçerlidir; yeni makbuzun formatı henüz kararlaştırılmadı.
- HistoryDeliveryCoordinator onayı önce RAM selective state/watermark'a uygular; kalıcı checkpoint ayrı işlemdir. Kesinti/yeniden başlatma ve güvenli Flash serbest bırakma testleri zorunlu.
- ObservationStore formatında PRODUCT_SECURE için tam dört exact-object önbellek yuvası var. TrackerProductTxPilot selectLive bunu doldurduğunda engellenebilir. **Eski Flash kayıtları ve güvenlik kanıtlarını silerek çözüm yok**; yeni rutin LIVE hattı buna bağlı olmayacak.
- SF5L0 bench kodu (main.cpp) bench_pacer.awaitingAck() sırasında canlı v1 konumu bastırıyor; bu deneysel radyo çakışması korumasıdır, yeni ürün LIVE politikasına taşınmamalı.
- GatewayProductCustodyRuntime bir bekleyen custody ACK nedeniyle yeni PRODUCT_SECURE kabulünü bekletebilir. Yeni makbuz sistemi tek bir ACK için genel alım hattını durdurmamalı.

## Örnek ve güvenlik

101,102,103,104,105 gönderildi, Gateway 103'ü alamadı. Kalıcı delil yalnız 101,102,104,105 içinse makbuz da bunları kapsar; 103 silinmez. 106 LIVE, 103'ün makbuzunu beklemez. Makbuzdaki sadece büyük sıra numarasına göre otomatik bir aralık teslim edilmiş sayılmaz.

**Kritik:** Makbuzun kriptografik yetki/kapsam/replay kararı olmadan, hiçbir salt-sayı/küme ifadesi kalıcı serbest bırakma emri değildir. Host modelinin authenticated=True parametresi gerçek güvenlik kontrolü değildir.

## Sayısal PC modeli (ölçüm veya saha kapasitesi değil)

Dört tasma, 1 dakika rapor, SF11, sıfır kayıp/tekrar ve #305 360000ms/saat Gateway yayın bütçesi:
- Her kayıt için ayrı 56 bayt ACK: 240 * 1397ms = **335280ms/saat**, ~%93,1.
- Beş dakikada 5 kayıtlık mevcut 88 bayt HistorySecure BACKEND_DURABLE makbuzu boyutunu örnek alırsak: 48 * 1971ms = **94608ms/saat**, ~%26,3.
- Uplink, gerçek receipt yetkisi, yeniden gönderim, bağlantı kesintisi, flash kapasitesi, gateway role ve radyo çakışmaları dahil değildir.

## PC komutları

python3 tools/kq05_receipt_model.py --test

python3 tools/kq05_receipt_model.py

Mock model sınırsız Flash değildir: kapasite dolduğunda açık FLASH_FULL üretir. 48 saatlik 1 dakikalık senaryo 2880 kaydı tutacak **varsayımsal** bir bellekte denenir; gerçek RAK flash kapasitesi böyle ispatlanmış değildir.

## Aşamalı kabul

- [ ] KQ-05A: Üç hatın protokol sözleşmesi, host model, kaynak audit; gerçek Flash/güvenlik eşlemesi.
- [ ] KQ-05B: Şifreli ve kimliği doğrulanmış LIVE; 4/4 cache doluluğundan bağımsız en güncel TX.
- [ ] KQ-05C: Yetkili toplu makbuzun gerçek on-air güvenliği (AEAD/HMAC, replay, device, incarnation, epoch, exact record binding), doğru backend/Gateway authority.
- [ ] KQ-05D: Flash release ve checkpoint, enerji kesintisi/rollback/migrasyon, yıpranma ve doluluk uyarısı.
- [ ] KQ-05E: 1/2/4 tasma, 1/2/5/15/60dk, %0/5/10/20 kayıp, 24–48h offline, latest-first fairness, 4/4 eski cache ve RF hesapları.
- [ ] #221 ürün kapısı ve #305 bütçe PR entegrasyonu, gerçek donanım/anahtar kabulü.

Mevcut 2/5/60 dakika eski firmware pil denemeleri sürerken **cihazlara flash, çalışan servislere/DB'ye müdahale ve production main merge yok**.
