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

## KQ-05B ilk gerçek firmware kod dilimi (taslak, etkin DEĞİL)

- `TrackerAcklessLiveTx` doğrudan `ProductRecordStore` içindeki en yeni kalıcı PERIODIC kaydı okur, `ProductSecureCrypto` ile yeni nonce ve AEAD korumalı `PRODUCT_SECURE` çerçeve üretir, `RadioManager` üzerinden gönderir. **Eski dört exact-object önbelleğine yazmaz; önceki paketin ACK'sini beklemez.**
- `path_flags=0` (doğrudan canlı bilgi; **custody talebi değil**); tasmanın kalıcı kaydı serbest bırakılmaz. RF kabul reddinde 30 saniye geri çekilir; bu sürede nonce yakarak tekrar tekrar denemez. Yeni kayıt eski canlı adayı geçer.
- Mevcut `GatewayCustodyAuthorizedIngress` **custody istemeyen çerçeveyi reddeder**. Gerçek Gateway kalıcı kayıt kabulü, AEAD çözümü, deduplikasyon, seçmeli makbuz ve Flash release/checkpoint henüz yoktur. **Bu yüzden bu gönderim tek başına uçtan uca çalışmaz.**
- Deneysel yol yalnız `ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE` ile açılır; `ORUN_SF5K3V_TRACKER_TX_PILOT` da gerektirir. Normal `rak4630` imajında kesinlikle kapalıdır. Eski custody pilotu derleme bayrağı kapalıysa aynı kalır.
- `test_sf5k3v_tracker_pilot.cpp` kapsamına gerçek ProductRecordStore fake Flash üzerinde dolu **4/4** cache, yeni 5. ve 7. konumun önbellek dışı gönderimi, ret/backoff ve false release olmaması regresyonları eklenir. Fake `ProductSecureCrypto` gerçek AEAD değildir: fiziksel/kriptografik kabul hâlâ ayrı.

**Geçiş siyaseti:** Kullanıcı eski test kayıtlarının temizlenmesini kabul etti; bu gelecekte kontrollü temiz geçişe izin verir ama mevcut cihaz Flash'ını veya backend'i otomatik silme yetkisi değildir. Herhangi bir silmeden önce hangi cihaz ve hangi kayıtların temizleneceği ayrı planlanacak.

## KQ-05B2 — Gateway non-custody staging (henüz entegre radyo çalışma değil)

- `ProductCustodyStore::requestLive()` yalnız custody bit'i **kapalı** PRODUCT_SECURE kabul eder; eski `requestCustody()` hâlâ custody bit'i **açık** olmayanı reddeder. Mevcut OCQ2 kaydı aynı biçimde saklar ve reboottan sonra tekrar okur.
- `GatewayProductLiveIngress` ilgili cihaz/epoch/Gateway için aktif scoped grant ile açıkça yetki arar, sonra opaque paketi no-eviction OCQ2 Flash'a yazar ve bağımsız readback kontrol eder. Paket bütünlüğünün AEAD çözümü ancak root credential sahibi Backend'de ispatlanabilir; grant yalnızca kaynak-admission filtresidir. Bu noktada **tekil ACK de toplu receipt de gönderilmez** ve teslim teminatı verilmez.
- Host fake NOR testine `no ACK`, flash readback, tekrar, reboot, geçersiz grant/çerçeve regresyonları eklendi. **Gerçek PC çalıştırma raporu henüz yok**.
- Deneysel Gateway radio mailbox yönlendirmesi, Edge durably intake ve kriptografik receipt/Tracker Flash release yapılmadan gerçek cihaz/aktif servis kullanımına **açmayın**.

## KQ-05B3 — yalnız deneysel Gateway RF yolu

- ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE etkinse noncustody PRODUCT_SECURE RX, scoped GatewayProductLiveIngress üzerinden OCQ2 Flash'a yönlendirilir. Varsayılan firmware yoluna etkisi yoktur.
- Eski custody ACK duty/driver nedeniyle beklerken, custody Flash yazısı artık tamamlanmışsa yeni LIVE kaydının kabulü engellenmez. Ayrı iki Flash yazısı aynı anda başlatılmaz.
- sf5l0_bench_gateway yeni LIVE durably stored olayında Edge export taramasını uyandırır, ancak Edge/backend güvenli ürün-alım uyumu, seçmeli makbuz ve tasma Flash release hâlâ tamamlanmamıştır.
- Host mock-radio testine ACK bütçe engeli altında canlı kayıt kabulü ve yalnız eski ACK'nin radyoya çıkışı eklenmiştir. Sonuçlar PC'de çalıştırılmadan PASS denmeyecek.

## KQ-05C — Backend verified selective receipt / tasma Flash release (14 Kasım)

- Backend gerçek `VerifiedLocalLedger` içindeki **doğrulanmış ve çatışmasız** kayıtları ayrı SQLite okuma oturumunda AEAD doğrulamasından geçirip yalnız istenen kimlikler için en fazla 8 kayıtlık HMAC makbuzu imzalayabiliyor. `backend/candidates/kq05_backend_batch_receipt.py` **host-only**; sunucunun HTTP cevabına henüz entegre edilmedi.
- Wire **deneysel**: `v2,type=9,flags=0,count,device[8],incarnation[8],key_epoch[4],sequences[N]*4,HMAC16`, 44–72 bayt. Sabit maksimum 8, sıralı/benzersiz/pozitif sequence; **aralık veya otomatik contiguous ACK yok**. Örn. 1,2,4,5 makbuzu 3'ü silmez.
- Backend-only anahtar: K_root + credential ID'den HKDF-SHA256 ile ayrı `ORUN-KQ05-BATCH-KEY-V1` anahtar alanı. Gateway K_root taşımadığı için kendi başına güvenli **mantıksal kayıt kimliği** makbuzu düzenleyemez. `kq05_backend_receipt.cpp` gerçek `SecurityStore` içinde CC310 bounded HKDF/HMAC ile MAC'i, epoch'u, cihaz kimliğini ve kalıcı incarnation'ı kontrol eder.
- `Kq05BackendReceiptReleaseOwner` tekil Flash mutation/readback ile yalnız gerçekten makbuzda yazılı retained PERIODIC OBS1 kayıtlarını serbest bırakıyor; dört yuvalı exact-byte ciphertext cache kullanılmıyor. Eski/yanlış MAC ve yabancı incarnation fail-closed. Tekrar (replay) aynı özgün kimliği yeniden serbest bırakamaz; geçmiş tekrarına karşı koruma **yalnız incarnation benzersizliği + idempotent release** varsayımıyla sınırlı. Ürün seviyesinde credential lifecycle / A2D replay sahipliği ayrıca denetlenmeli.
- `RadioManager` ve `main.cpp` **yalnız ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE** altında 44–72B yapısal makbuzu tasmanın RX mailbox'ına, ardından güvenli release owner'a yönlendirir. Hiçbir packet tek başına release yetkisi değildir.
- Host regresyonu: Python gerçek Backend SQLite/AES-CCM/HKDF/HMAC + kaynaklarda C++ test fixture (bağımsız Python oracle/gerçek SecurityStore/fake NOR, tam 4/4 cache tıkanma ve eksik 3). **Bu branch'in PC'de test/build çalıştırıldığına dair kayıt hâlâ YOK.**
- **Eksikler:** Backend makbuzunu yetkili Edge'e otomatik ileten yanıt/queue, Edge→Gateway kontrollü aktarım, Gateway RF downlink/rate/duty/uyku pencere zamanlaması, gerçek RAK/USB/BLE güvenlik ve fiziksel test, 24–48 saat offline kapasite, olay (EVENT) serbest bırakma, #305 RF bütçesi ile bütünleşme. Bu nedenle PR #316 **DRAFT / NO MERGE / NO FLASH**.

### İlk PC doğrulama komutları
```bash
bash tools/ci/run_sf5k3v_tracker_tx_host.sh
bash tools/ci/run_sf5k3w_gateway_product_runtime_host.sh
python3 firmware/tests/sf5/test_kq05_backend_batch_receipt.py
bash tools/ci/run_kq05_backend_receipt_release_host.sh
# Sonra tamamı:
bash firmware/tests/run_host_tests.sh
cd firmware && pio run -e rak4630
```

Eski test kayıtları daha sonra kullanıcı onayıyla kontrollü silinebilir; bu kod hiçbir gerçek cihaz belleğini, veritabanını veya anahtarı silmedi.
