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


## 14 Kasım — KQ05 ACKLESS + HISTORY / retry uygulama kontrolü (LAB ONLY)

- ACKless LIVE deneysel ve varsayılan kapalıdır. HISTORY `TrackerAcklessLiveTx::updateHistory()` üzerinden son LIVE denemesinden sonra çalışır. 8 kimliklik azami tarama dilimi ve 60 saniyelik geçmiş TX aralığı vardır; 5 dakikalık tam tarama beklemesi tekrar selini önler. **Bunlar yasaya uygun ortak RF token bütçesi değildir.** #305 tüm yayınları kapsayan gerçek RF kotasını sağlamadan sahada açılmaz.
- KQ05 Tracker RX mailbox artık kalıcı durum kontrolü başarısızken 5 saniye aralıkla, en çok 12 defa denemeye açık kalır; süresi dolunca yeni makbuz talebi gerekir. Kimlik/HMAC retleri hemen düşürülür. Bu **Backend'in 5 dakikalık yeniden makbuz üretimini veya RF downlink'i kurmaz**.
- PC denetimi: `bash tools/ci/run_kq05_lab_preflight.sh`. Ayrı `rak4630_kq05_lab` derleme hedefi vardır, cihaz flash'ı yapılmaz. KQ05 yeni kodu için PC çıktısı olmadığı sürece PASS yazılmaz.

### Fiziksel kapasite sınırı — ürün engeli

- `storage_config::kProductRegionPages=32`; `osf::kControlPageCount=2`; `osf::kDataRecordsPerPage=42`. Teorik azami PERIODIC veri sayısı `(32-2)*42 = 1260` kayıttır (yaklaşık **21 saat**, 1 dakika/rapor). Kontrol/rotasyon, EVENT ve yıpranma yükleri pratik kullanılabilir süreyi azaltabilir. **24–48 saat çevrimdışı tam veri korunumu bu Flash yerleşimiyle mümkün değildir.**
- Gateway OCQ2 üst sınırı 32 sayfa x 33 kayıt = 1056 çerçeve. Dört tasma, 1 dk periyotta, Gateway'den Edge'e handoff/reclaim olmadığında azami **4,4 saatlik** doluluk penceresidir.
- Çözüm için gerçek hafıza envanteri, veri saklama hedefi ve güvenli migration kararı gerekir. Gözetimsiz otomatik silme/overwrite veya varsayımsal 48 saat kapasite beyanı yapılmayacak.
- HISTORY EVENT veri tipini henüz desteklemez. Backend→Edge queue→Gateway RF→Tracker receipt otomatik dönüşü ve fiziksel failover/replay/RF bütçesi hâlâ **P0 BLOCKER** durumundadır. Bu tamamlanmadan #316 merge/flash yoktur.

## 17 Kasım — KQ05 Backend receipt outbox (ayrı LAB adımı)

Operatör kararı: **tekli CUSTODY_ACK firmware'de kalır**; rutin LIVE için ACKless yol ve Backend makbuzu yalnız açık deneysel rak4630_kq05_lab imajında denenir. Varsayılan rak4630, rak4630_orun, cihazlar, servisler, veritabanları ve ana dal değişmez. PR #316 **DRAFT / NO MERGE / NO FLASH**.

- backend/candidates/kq05_backend_receipt_outbox.py yalnız Backend'de, VerifiedLocalLedger ile aynı SQLite WAL veritabanında sınırlı kq05_receipt_outbox tablosu oluşturur. BEGIN IMMEDIATE rezervasyonu altında gerçek BackendBatchIssuer.issue() çağrısının AEAD/kimlik/çatışma/HMAC kontrolünü ve satır eklemeyi sıralar; bağımsız bağlantıyla satırın birebir okunduğunu doğrular.
- Aynı cihaz/incarnation/epoch/açık sequence kümesi tekrar eklendiğinde aynı satır döner. Kayıp/çatışmalı kayıt hiç sıraya alınmaz; bozuk outbox satırı veya yeni tespit edilen ledger çatışması, bütün oldest() okumasını REFUSED yapar (limite takılıp sadece temiz prefix döndürmez).
- Kuyruk dolduğunda FULL döner. Silme, teslim işaretleme, RF gönderme, ACK kabulü, telefon/Edge HTTP endpoint'i, kullanıcı yetkilendirmesi veya üretim credentials yönetimi YOK. oldest() tekrar çağrıldığında aynı makbuzları döndürmesi tasmanın bunları aldığı/uyguladığı anlamına gelmez.
- firmware/tests/sf5/test_kq05_backend_receipt_outbox.py gerçek test fixture'larıyla kimlik, seçmeli boşluk, kalıcılık, idempotence, sıra sınırı, bozuk MAC ve daha sonra oluşan çatışma korumasını sınamak üzere eklendi. Hem run_kq05_lab_preflight.sh hem tam host regresyonuna dahil edildi.
- **Yeni commit'in operatör makinesindeki test sonucu henüz alınmadı; PASS denmeyecek.** Ayrı local davranış-smoke koşuldu; bu gerçek Backend AEAD fixture'ının veya target LAB build'in yerine geçmez.
- **Sonraki kapı:** Yetkili Backend → Edge makbuz taşıma protokolü, kalıcı Edge kuyruk/retry, Edge → Gateway güvenlik, Gateway → Tracker RF downlink/uyku pencere ve #305 airtime bütçesi. Tekli ACK korunarak fiziksel kabul gelmeden firmware yüklenmez.

## 17 Kasım — KQ05 Backend → Edge imzalı sayfalar / kalıcı Edge kuyruğu

- Ayrı deneysel kaynaklar: backend/candidates/kq05_receipt_edge_transport.py ve edge/candidates/kq05_receipt_inbox.py. Canlı Backend HTTP server veya Android Edge uygulaması değiştirilmez.
- Backend yalnız kayıtlı edge_id için ayrı 32B taşıma HMAC anahtarına ve Backend tarafından belirlenen izinli Tracker ID listesine dayanır. İstek edge/after/limit/random nonce ile HMAC'lenir; yanıt o isteğin nonce ve cursor'ına HMAC ile bağlanır. Bu uygulama, üretimde bağımsız enrollment ve TLS gerektirir; ortak bench bearer anahtarı yerine kullanılacak hazır üretim yetki servisi değildir.
- Backend kuyruğu sayfa okumasında tüm kayıtları gözden geçirip her makbuz için yeniden AEAD/HMAC/çatışma kanıtı arar, geçersiz satır varsa tamamen reddeder. Yalnız kayıtlı Edge'e atanmış cihazlar listelenir; cursor en son verilen kayıt ID'si olur, hiçbir teslim/silme işareti üretilmez.
- Edge kopyasında aynı installation kimliği, giriş mesajı HMAC'i, hedef cihaz/epoch/sequence biçimi, büyüklük, artan ID/sayfa sınırı, SQLite atomic cursor+records ve yeniden başlatma readback kontrolü vardır. Inbox satırları Edge'e ait transfer HMAC'iyle etiketlenir; içeride bir MAC bile bozulursa bütün tarama REFUSED olur. Bu Edge'in Tracker K_root veya Backend RECEIPT HMAC üretim yetkisine sahip olduğu anlamına gelmez.
- Yeni host testi firmware/tests/sf5/test_kq05_receipt_edge_transport.py; run_host_tests.sh ve run_kq05_lab_preflight.sh içine dahil. **Bu güncel HEAD için operatör PC sonucu henüz yok; PASS veya uçtan uca RF başarı iddiası yapılmaz.**
- Açık kapılar: üretim hesap/Edge enrollment ve TLS entegrasyonu, gerçek Backend HTTP bağlama, TypeScript/PC Edge çalışma kuyrukları, Edge→Gateway kalıcı aktarım, Gateway→Tracker RF downlink ve #305 ortak bütçe, EVENT release, cihaz pil/flash/saha kabulü.
- Eski tekli CUSTODY_ACK, komut onayları, default firmware ve servisler olduğu gibi kalır. PR taslaktır: NO MERGE / NO FLASH.

## 17 Kasım — KQ05 Edge → Gateway receipt staging (LAB)

- Yeni modül edge/candidates/kq05_edge_gateway_offer.py: sadece kalıcı EdgeReceiptInbox taraması doğrulandığında, 8B gateway kimliği / Edge satır ID / 44..72B asıl Backend makbuzunu farklı bir Edge-Gateway bağlantı anahtarıyla HMAC-SHA256'ya bağlayan 95..123B deneysel mesaj üretir. İletişim kaydı 160 baytlık mevcut Edge BLE logical sınırını aşmaz; bu, gerçek BLE aktarımının uygulandığı anlamına gelmez.
- gateway/candidates/kq05_gateway_receipt_stage.py: Gateway rolünü modelleyen ayrı host SQLite WAL/synchronous=FULL kalıcı inbox. Kayıtlar gateway ve Edge kurulumuna/anahtarına bağlanır; tekrar ve aynı ID farklı içerik reddedilir; kapasite dolunca sessiz silme yoktur. Bütün taramada MAC, cihaz yetkisi ve boyut denetimi yapılır; sağlıklı prefix'i bozuk suffix'ten koparıp başarılı göstermez.
- firmware/tests/sf5/test_kq05_edge_gateway_stage.py: gerçek Backend → Edge makbuz fixture'ından Gateway staging'e kadar işlem; yanlış Edge/gateway/key/device, tamper, satır çakışması, yeniden başlatma, FULL, Edge ve Gateway SQLite corruption, tekrar ve Backend değişmeden kalma kontrolü. Hem tam host suite hem KQ05 lab preflight çağırır.
- **Henüz gerçek USB/BLE mesaj tipi, nRF Gateway flash writer, RF TX, alım pencere planlaması, #305 kota, Tracker makbuz alma callback, gerçek cihaz testi YOK.** Gateway'in Edge bağlantı MAC'i Backend'in receipt HMAC'ini doğrulamaz; yalnız Tracker gerçek credential ile yapabilir. Üretim şifresi veya kayıt bırakma yetkisi Gateway'de bulunmaz.
- Ana firmware, gerçek servis/DB, bağlı cihazlar değişmedi. Tekli CUSTODY_ACK korunur. DRAFT / NO MERGE / NO FLASH. Güncel SHA için operatör PC test sonucu beklenir.

## 17 Kasım — KQ05 Gateway durable-stage result (host LAB)

- Gateway makbuzu bağımsız SQLite COMMIT/readback yaptıktan sonra, aynı Edge-Gateway bağlantı HMAC anahtarıyla Gateway kimliği + kaynak Edge satırı + ilgili teklifin SHA-256 hash'i + durum kodu için 83B sabit yanıt üretebilir (gateway/candidates/kq05_gateway_stage_ack.py).
- Edge gelen yanıtı yalnız aynı teklife, kurulum kimliğine ve doğru bağlantı anahtarına bağlıysa kabul eder; yeniden başlatma sonrası aynı kayıt için DUPLICATE ayrı imzalanır. Bozuk token, farklı teklif/gateway/edge, dolu veya bozulmuş queue için başarılı yanıt yoktur.
- Bu olayın adı **Gateway STAGED**; eski tekli CUSTODY_ACK değildir. Backend'e teslim, RF gönderimi, Tracker'ın kayıt bırakması veya Edge/Backend kuyruğundan silme izni sağlamaz. Tasma kayıtları sadece kendi gerçek credential tabanlı Backend toplu makbuz doğrulaması sonrası serbest bırakır.
- firmware/tests/sf5/test_kq05_gateway_stage_ack.py, KQ05 preflight ve tam host regresyonuna eklendi: HMAC bit-fuzz, yanlış teklif/identity, kalıcı tekrar/reboot, FULL ve corrupt SQLite. Yeni sürüm için operatör PC testleri **henüz çalıştırılmadı**.
- Kritik sonraki adım: ürün Edge USB/BLE aktarımı, Gateway nRF Flash'a kalıcı yazım, Gateway LoRa TX scheduler (#305 ile paylaşılan airtime) ve Tracker receive path fiziksel pilot. Default firmware/ana dal/cihaz/servis korunur. Tekli ACK varlığını sürdürür; DRAFT / NO MERGE / NO FLASH.

## 17 Kasım — KQ05 SF5L6 BLE on-wire köprüsü (kalıcı Gateway owner bekliyor)

- firmware/include/kq05_edge_link_wire.h ve firmware/src/kq05_edge_link_wire.cpp: yalnız ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE derlemesinde, mevcut SF5L6 edge_link 160B logical limitine uygun 0x91 (Edge→Gateway receipt offer) ve 0x08 (Gateway→Edge STAGED result) dış envelope, tam 95..123B iç teklif / 83B sonuç geometrisi ile native C++ structural inspection. 0x91 dış envelope+19B header+44..72B Backend receipt+32B Edge link tag => 96..124B; STAGED dış envelope => 84B.
- Deneysel sf5l0_bench_gateway.cpp BLE reassembly handler'ı 0x91'i tanıyıp, gerçek HMAC yetkisi ve nRF Gateway Flash owner bulunmadığından güvenli biçimde REJECT ediyor. **Bu fiziksel BLE aktarımın tamamlandığı anlamına gelmez**; hiçbir STAGED/SENT/ACK/Tracker release/Flash mutation yapılmıyor.
- firmware/tests/sf5/test_kq05_edge_link_wire.cpp ve tools/ci/run_kq05_edge_link_wire_host.sh mevcut real edge_link::Fragmenter/Reassembler ile 20B BLE fragmentlerine bölme/toplama, minimum/maksimum mesaj, parser malformed ve eski mesaj türü ayrımını sınar. KQ05 preflight ve tüm host tests içine eklendi. Bu değişiklikler için operatör PC ve deneysel firmware derleme sonucu HENÜZ YOK; PASS denemez.
- Eski tekli CUSTODY_ACK ve LoRa komutları korunur. Devam işi: üretime uygun Edge USB/BLE key enrollment, Gateway nRF kalıcı receipt owner + readback, authentication, Gateway stage response, #305 toplam RF budget, LoRa TX ve Tracker RX. PR #316 DRAFT / NO MERGE / NO FLASH; ana firmware/servisler/cihazlar değiştirilmez.

## 17 Kasım — KQ05 BLE LAB PASS ve Gateway Flash yerleşim engeli

### Operatör PC kabulü (SHA 78819a68a1fe6210af9ab5d2abb3355d332d1d6e)

- KQ05 preflight, gerçek SF5L6 BLE 20B native fragment/reassembly testi ve `pio run -e rak4630_kq05_lab`: **PASS/SUCCESS**. Flash 468.908/815.104 (%57,5), RAM 51.192/248.832 (%20,6). Tekli ACK korunuyor; cihaz flash/yükleme yok. Bu çalıştırmada tüm host regression ayrı çağrılmadı.

### Kritik fiziksel depolama gerçeği (karar kapısı)

- `storage_config.h`: uygulama son sınırı **0x0C5000**; ürün bölgesi **0x0C5000..0x0E5000 (32 x 4096 = 128 KiB)**; bunun hemen üzerinde Geofence, Security, Config, Bond ve History özel sahipli alanları yer alıyor. Ayrı bir boş/rezervsiz Gateway makbuz sayfası yok.
- `product_region.h`, `product_custody_store.h/.cpp`: Gateway için ürün bölgesinin TAMAMI `ProductCustodyStore` OCQ2'ye ait. Açılış taraması ve en son nesil/aktif sayfa kuralları bütün sayfaları denetliyor; ortadan 1-2 sayfa ayırmak, kapasite/format/sahiplik/kurtarma/migrasyon ve erase güvenliği yeniden tasarlanmadan **güvenli değil**.
- `FlashMutationGate::productPort()` ürün bölgesinin *fiziksel mutasyon arbitrajı* sahibi; tek başına makbuz için mantıksal tahsis veya OCQ2 ile bağımsız yazma yetkisi vermez. Yeni rastgele writer veya kısmi bölge erase önerilmiyor.

### Önerilen daha basit rota (henüz ürün uygulama kararı değil)

1. Backend ve Edge makbuzları kalıcı tutar; Edge, gerçek tasma doğrulama/işleme kanıtı olmadan makbuzu silmez.
2. Yetkilendirilmiş Edge→Gateway BLE/USB taşıması Gateway'e yalnız kısa ömürlü, sınırlı RAM offer verir; ACK/komutlar ve yeni LIVE öncelikli kalır; Gateway kendi ek Flash kaydını üretmez. Düşen bağlantıda Edge yeniden dener.
3. Gateway yalnız Tracker'ın açık RX penceresinde ve doğrulanmış #305 RF-airtime bütçesinde makbuzun **orijinal Backend-signed 44..72B wire** kısmını downlink'e çıkarır. Gateway anahtarı Backend receipt HMAC'i üretmez/doğrulamaz.
4. Tasma receipt'i kendi SecurityStore credential'ıyla HMAC/scope/epoch/sequence doğrulayarak yalnız seçilen OBS1 kayıtlarını idempotent biçimde bırakır; eski tekli ACK yüksek öncelikli olay/komut işlerinde kalır.
5. Bu rota seçilirse host-only SQLite Gateway `STAGED` ve 83B dayanıklı saklama onayı **gerçek Gateway firmware'de kullanılmaz**. RAM teklifine asla `STAGED` denmez; gerekiyorsa sadece 'RAM QUEUED/NOT DURABLE' olarak tanımlanır; bu cevap Edge veya Backend'de silme yetkisi oluşturmaz.

**Karar durumu:** Yukarıdaki RAM relay sadeleştirme mühendislik ÖNERİSİ; nRF ürün yolu uygulanmadı. Alternatif kalıcı Gateway makbuz kuyruğu istenirse, OCQ2 içinde formata uyumlu yeni kayıt tipi + geriye uyumlu recovery/migration ve flash cut-test gerektiren ayrı depolama projesidir. Bu iş açık kalır; veri silme veya sahiplik değişikliği yapılmaz.

### Açık saha kapıları

- Gerçek Edge/Gateway credential ve bağlantı yetkisi, USB/BLE kablosuz alma handler'ı, bounded RAM ömrü ve retry, radio-owner TX scheduler, LoRa duty-cycle (#305), tasma RX pencere/zaman hesabı, fiziksel iki cihaz testi ve elektrik tüketimi/power-cut kanıtı.
- PR #316 DRAFT; NO MERGE / NO FLASH / NO MAIN. Tekli ACK değiştirilmez.

## 17 Kasım — KQ05 Gateway RAM relay çekirdeği (native C++, henüz RF değil)

Gateway ürünü için OCQ2 Flash bölgesinden sayfa ayırmama kararı korunmuştur. Yeni `kq05_gateway_ram_relay.h/.cpp` yalnız `ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE` altında derlenebilen, en fazla **iki adet 72B orijinal Backend makbuzu** tutan kısa ömürlü RAM rölesidir.

- Makbuz `kq05_edge_link_wire::inspectOffer` ile yalnız şekil açısından açılır; ayrıca ayrı **Edge–Gateway** HMAC-SHA256, kayıtlı Edge installation kimliği, Gateway hedefi, cihaz scope'u ve *yerel kalıcı/revocable yetki kaynağının* doğrulanması zorunludur. Python protokolündeki `ORUN-KQ05-EDGE-GATEWAY-OFFER-v1\0 || edge-id || \0 || inner unsigned offer` HMAC hesaplanır. Gateway **Backend–Tracker HMAC'ini üretmez/doğrulamaz**, Tracker K_root'a erişmez.
- `CredentialSource` şu anda yalnız soyut, güvenilir bir sözleşmedir; **gerçek Gateway enrollment/kalıcı kimlik anahtarı kaynağı henüz uygulanmadı**. Dolayısıyla canlı BLE `edgeMessage()` yolu hâlâ güvenli biçimde reddeder; yeni sınıf otomatik aktive edilmedi.
- Kayıt başına maksimum 20 saniyelik TTL, aynı kaynak kayıt numarası için birebir dedupe/çakışma reddi, iki slot dolunca `FULL`, yetki kalkınca tüm RAM'i sıfırlama ve rollover-safe zaman hesabı bulunur. **RAM dolması/yeniden başlaması makbuz teslimi değildir**; Edge/Backend kalıcı saklama zorunluluğu devam eder.
- İleriye dönük `select()` yalnız yetkili Gateway, boş radyo, **tekli CUSTODY_ACK beklemiyor**, komut beklemiyor, canlı trafik beklemiyor, gerçek RF kotası verilmiş, tasmanın alım penceresi açık ve son 3000ms içinde cihaz görülmüşse yalnız *aday* çıkarır. Bu boolean kapılar üst seviyede henüz fiziksel kaynaklarla bağlanmadı; `select()` radyo TX gerçekleştirmez. `markTxStarted()` yalnız gerçek RF gönderimi başlatıldığında deneme aralığı koymak için tasarlandı, teslim onayı değildir.
- `firmware/tests/sf5/test_kq05_gateway_ram_relay.cpp` ve `tools/ci/run_kq05_gateway_ram_relay_host.sh` native OpenSSL host portu ile gerçek HMAC input sözleşmesini, yanlış cihaz/gateway/key, kapasite, TTL/32-bit wrap, geri alma ve ACK/komut/LIVE/RF bütçesi önceliklerini denemek üzere eklendi. KQ05 preflight ve tam host suite içine konuldu.
- **Bu yeni commit için operatör PC sonucu henüz alınmadı; PASS ilan edilmez.** Tamamlama kapıları: fiziksel yetki/provisioning, gerçek BLE/USB paket girişine RAM relay bağlantısı, gerçek RF airtime bütçe otoritesi (#305), radio-owner downlink, tasma RX kanıtı, gerçek RAK testleri ve güç/power cut.
- Ana firmware, gerçek servisler/DB ve bağlı RAK cihazları değiştirilmedi. Tekli ACK kodu korunur, PR #316 DRAFT / NO MERGE / NO FLASH.

## 17 Kasım — KQ05 OGK1 v2 Edge-Gateway yetkili bağlantı anahtarı (LAB)

- Kullanıcı PC'de bir önceki `5199c4f` KQ05 RAM röle ve LAB preflight testlerinin PASS olduğunu, RAK4630 deneysel hedef derlemesinin RAM %20,6 / Flash %57,5 SUCCESS olduğunu doğruladı. Gerçek RAK firmware yüklenmedi; bu PC PASS saha kanıtı değildir.
- Yeni `firmware/include/kq05_gateway_signed_link_credentials.h` + `firmware/src/kq05_gateway_signed_link_credentials.cpp`: yalnız doğrulanmış `GatewayGrantSnapshotReader` **OGK1 v2**'nin özel `edgeAcceptKey` kaynağından KQ05'e ayrı amaçlı **HMAC-SHA256** bağlantı anahtarı üretir. `OGK1 v1`, eşleşmeyen Gateway kimliği, hatalı/boş installation ID, iptal edilmiş/bozulmuş signed authority **fail-closed**. Mevcut Edge ACCEPT anahtarı KQ05 offer MAC'inde doğrudan tekrar kullanılmaz.
- Gateway anahtar türetimi: HMAC(OGK1-v2 Edge ACCEPT 32B key, ASCII `ORUN-KQ05-OGK1-EDGE-LINK-DERIVE-v1` + NUL + gateway ID (8B BE) + güvenilir Edge installation ID (1..64B ASCII)). `edge/candidates/kq05_gateway_link_derive.py` aynı bağımsız Edge formülünü uygular. KAT: OGK1 key 0x40..0x5F, gateway ASCII `GATEWAY1`, Edge ID `edge-one` -> `cfa987ef239b763e3dc1b05d3f7bd3bda8c675659ab0c1935d386e1e466ba84f`. Bu test vektörü GERÇEK CİHAZ kimlik bilgisi değildir.
- `GatewayRamRelay` artık OGK1 grantini yalnız tasma kimliğiyle değil **makbuzun tam `key_epoch` değeriyle** eşleştirir; yanlış epoch valid HMAC'e sahip olsa bile reddedilir. Alım sonrası kriptografik yetki tekrar kontrol edilir, `select()` reddinde önceki candidate temizlenir.
- Native test: `firmware/tests/sf5/test_kq05_gateway_signed_link_credentials.cpp` sahte issuer proof + gerçek OGK1 reader + gerçek OpenSSL HMAC/relay sınıflarını sınar; yanlış epoch, Edge ID, Gateway, trust iptali ve NUL/key derivation. Python aynı vektörü bağımsız doğrular. KQ05 preflight ve tam host testlerine eklendi.
- **Bu kod henüz BLE Gateway girişine canlı bağlanmadı:** güvenilir Edge installation kimliğinin hesap/cihaz eşleştirmesinden gelmesi, key rotation, gerçek signed provisioning ve fiziksel kanıt gereklidir. Sırf geçerli OGK1 snapshot veya test ID var diye otomatik BLE kabulü açılamaz. Gateway girişindeki REJECT korunur.
- **Güncel deneysel HEAD için PC çalıştırma sonucu henüz yok.** `main`, varsayılan firmware, servis/DB ve RAK cihazları değişmedi. Tekli CUSTODY_ACK korunur. PR #316 DRAFT / NO MERGE / NO FLASH.

## KQ05 gerçek bayt uyumluluğu — Backend → Edge → BLE → Gateway RAM (LAB)

- Operatör `55789c7d20714c9b84d2242f391355ad8d6030cb` SHA'sında Gateway RAM native HMAC/scope/priority/expiry, OGK1 v2 Edge/C++ key derivation KAT, signed snapshot fake issuer grant/epoch ve Tracker real-credential receipt/Flash testlerini **PASS** raporladı; RAK4630 KQ05 LAB derleme **SUCCESS**, RAM %20,6 / Flash %57,5. **Cihaz flash yok**.
- Python `firmware/tests/sf5/generate_kq05_real_ble_offer.py`: gerçek AES-CCM ile onaylanmış Backend test kaydı, durable Backend outbox, authenticated Backend→Edge HMAC page ve SQLite Edge inbox'dan oluşan 1 makbuz üretir; kullanıcıya ait gerçek şifre kullanılmaz, sadece halka açık test credential/OGK1 fixture bulunur. Python, Edge→Gateway teklifini imzalar ve 0x91 logical BLE envelope ekleyerek tam 96 bayt yazdırır.
- C++ `firmware/tests/sf5/test_kq05_python_gateway_interop.cpp` ve `tools/ci/run_kq05_real_byte_interop_host.sh`: Python'un ürettiği aynı 96 byte mesajı gerçek SF5L6 `Fragmenter` ile 20 byte mesajlara ayırır, `Reassembler` ile toplar; mevcut `GatewayRamRelay` üzerinde Edge HMAC, tam cihaz/epoch scope, idempotent RAM admission, ACK-first seçim kapısı, değişmiş HMAC, iptal edilen yetki ve kaynak belgeyi silmeme kontrollerini yapar.
- Bu test GERÇEK Bluetooth hava aktarımı veya Gateway'e fiziksel giriş değildir. Native testteki `FakeSignedEnrollment` yalnız herkese açık 32B KAT'a sabitlenmiştir; bunun saha cihazlarında kullanılması YASAK. Gerçek BLE `edgeMessage` deneysel offer'ı halen **fail closed** reddeder; güvenilir Edge installation identity+provisioning ve RF owner gerekir.
- Yeni test tam host suite ve KQ05 LAB preflight akışına eklendi. **Bu güncel commit için operatör PC sonucu henüz yok; PASS iddiası yapılmaz.** Tekli ACK/Default firmware/cihazlar/servisler/DB değişmedi; PR #316 DRAFT / NO MERGE / NO FLASH.

## KQ05 Edge installation kimliği — güvenilir kaynak zorunluluğu

- `3c8b00fe3322d6f0fcad04e9396a2bf6a1e27003` üzerinde operatör `run_kq05_real_byte_interop_host.sh` **PASS** sonucunu kaydetti. Backend AES-CCM→Edge HMAC/SQLite→96B offer→gerçek SF5L6 native 20B frames→Gateway HMAC/RAM uyumlu (yalnız host). Bu test fiziksel BLE/RF değil.
- Yeni `TrustedEdgeInstallation` soyut sözleşmesi: `readVerifiedInstallation(gateway_id, char[65])` yalnız Edge kurulum kimliğini bağımsız, yerel ve geri alınabilir şekilde doğrulayan gerçek enrollment owner tarafından sağlanabilir. BLE GATT adından, karşı tarafın beyanından, test key'inden veya herhangi bir serbest metinden kimlik üretilemez.
- `SignedGrantCredentials` ctor artık `const char*` kabul etmez; `TrustedEdgeInstallation&` gerektirir. `OGK1 v2` imzalı kayıt ve bağımsız Edge kimliği ikisi de geçerli olmadan KQ05 key türetme yapılamaz. CC310 HMAC hesaplaması öncesi/sonrası kimlik tekrar okunur; değişirse anahtar gönderilmez. İptalde tüm yanıt anahtarları sıfırlanır.
- Native fake issuer ve fake identity testleri, yanlış kimlik, yanlış gateway, kimlik iptali ve RAM fail-closed senaryolarını kapsayacak şekilde güncellendi. Gerçek üretim `TrustedEdgeInstallation` implementation henüz YOK; BLE 0x91 girişi reddetmeyi sürdürüyor.
- **Bu yeni HEAD'in PC doğrulama sonucu henüz alınmadı.** `main`, varsayılan ürün firmware, servis/DB, cihazlar ve eski tekli CUSTODY_ACK değiştirilmedi; PR #316 DRAFT / NO MERGE / NO FLASH.

## 17 Kasım — Çift kayıt kök nedenine odaklı tek kaynak düzeltmesi (KQ05 LAB)

**Kök neden:** Aynı normal GNSS fix için `finishProductPeriod(&fix)` ile OBS1 `ProductRecordStore::submitPeriodic()` ve `positions.acceptFix()` ile eski `HistoryStore::append()` ayrı kayıt oluşturuyordu. Sonra hem eski `HistoryStoreForwardRuntime` hem KQ05 `TrackerAcklessLiveTx` farklı biçimlerde aynı dönem verisini yayınlayabiliyordu. Backend product-only dedup bunu düzeltmez: iki farklı kimlik/format için tekillik yeterli değildir.

**Mevcut çözüm kapsamı — yalnız `ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE` + Tracker storage rolü:**

- KQ05 Tracker'da `kq05ProductOnlyTracker()` gerçek rol kararıyla `true` olur. GNSS normal fix'i `finishProductPeriod` ve OBS1'e gider, **aynı fix'e `positions.acceptFix` çağrılmaz**. Test dışındaki firmware'de mevcut yol korunur.
- Eski `POSITION` canlı TX'i, HistoryStore backlog replay/receipt update'i ve `history.poll()` KQ05 Tracker'da durdurulur. Boot sırasındaki kritik düzeltme: `HistoryStore::begin` artık role kararından SONRA çağrılır ve KQ05 Tracker için hiç çağrılmaz; dolayısıyla boş eski bölgeye otomatik ilk sayfa hazırlığı bile yok. Eski HistoryStore Flash sayfaları **silinmez, migrate edilmez**; eski kayıtların kapalı kaldığı kullanıcıya açıklanmalıdır.
- KQ05 ilk ürün ölçüm periyodunu GNSS ilk fix'ten önce açar; `ProductRecordStore` hazır değilse normal fix için eski depoya düşmez (fail-closed). Periyot kabul edilmezse yanlış başarı ilan edilmez, periyot başlangıcı ve baseline ilerletilmez.
- Eski geofence representative fix saklaması KQ05 Tracker'da **kapatıldı** çünkü paralel HistoryStore yazıcısı yeniden çift kayıt üretirdi. Yeni OBS1 **EVENT üretimi ve kalıcı geofence alarmı henüz uygulanmadı**. KQ05 bu nedenle gerçek alan ihlali/alarm kabulü için release-ready değildir; `KQ05 GEOFENCE representative PRODUCT EVENT not implemented` logu çıkar. Kullanıcıya tamamlandı iddiası yapılmaz.
- `firmware/tests/sf5/test_kq05_single_product_owner_source.py`, KQ05 lab preflight ve full host suite'e eklendi; kaynak bağlama kararı statik kontrol edilir. Gerçek RAK4630 build, host integration/field flash ve normal GNSS→OBS1 sayım testi henüz bu HEAD için YAPILMADI.

**İş kapatma şartları:** 1) PC scriptin hedef derlemesi ve statik kontrolleri PASS; 2) PC native fake flash senaryosunda aynı GNSS ölçümü için bir ürün kaydı, sıfır yeni legacy kayıt ve no-fallback arızası kanıtlanmalı; 3) gerçek cihazda 1-dk/15-dk, reboot, no-fix, geofence, full storage ve power cut testleri; 4) eski geçmiş veri korunması/operatör export-migration kararı; 5) ürün EVENT/alarmlar, authenticated Edge BLE, Gateway RF duty/rx ve Tracker downlink bitirilmeden PR #316 ana dala alınmaz, saha etiketi verilmez.

**İş yönetimi:** Bundan sonra KQ05 için ayrı yeni özellik PR'ları açılmadan mevcut #316 altında tüm kapılar kapatılacaktır. PC PASS yalnızca ilgili SHA'yı doğrular; sözde kalan iş `DONE` diye işaretlenmez. Ana firmware, hizmetler, gerçek cihazlar ve eski ACK unchanged.
