# KQ05 — Tek Gateway ACK motoru, 1..5 kayıt (LAB / taslak)

**Tarih:** 19 Kasım 2026. **Durum:** DRAFT, gerçek RAK/LoRa doğrulaması YOK.
Başlangıç: PR #354. Bu geliştirme yalnız deneysel `ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF` altında çalışır; varsayılan `rak4630` ve `rak4630_kq05_lab` hedefleri değiştirilmez.

## Karar

Aynı Gateway custody ACK protokolü **0x0A** hem **tek kaydı (count=1, 56B)** hem **2..5 kaydı (64..88B)** imzalar/doğrular. İki farklı Gateway onay motoru bir arada çalıştırılmaz:

- Deneysel Gateway: tek `Kq05GatewayBatchCollector` + `Kq05GatewayBatchRuntime` + tek `RadioManager::sendAuthorizedKq05GatewayBatchAck`. Eski 0x05 Gateway custody runtime'ı bu derlemede kurulmaz.
- Deneysel Tracker: tek `Kq05TrackerBatchRadioRuntime` + `Kq05TrackerBatchReleaseOwner`. Eski `CustodyAckReleaseOwner` 0x05 alıcısı, RX / Flash service ve global nesnesi bu imajda **derleme dışında**. Default firmware'de eski 0x05 değişmeden kalır.
- Tekli ve toplu 0x0A aynı yetkilendirilmiş grant, tam SHA256, kapsamlı HMAC, exact Flash okuma, tek tek dayanıklı OBS1 release-marker yolundan geçer. **56B count=1 bile HMAC/doğrulama olmadan kayıt silmez.**
- **0x09 Backend-DURABLE makbuzu ayrı bir güvenlik sınırıdır.** Normal HISTORY için ana ürün hedefi Gateway'in OCQ2'ye dayalı yerel custody ACK'sidir; Backend 0x09 makbuzu tasmanın Gateway ACK'si için ön şart değildir.

## Uyarlanabilir politika (deneysel)

| Durum | Beklenen 0x0A |
| --- | --- |
| Kritik EVENT, Gateway OCQ2 Flash'a kalıcı yazdı | Hemen tekli count=1 flush (önce bekleyen farklı grubu flush et, olayı tekrar iste) |
| Önceden Gateway'e yazılmış exact kayıt ACK kaybıyla tekrar geldi | 30 saniyelik **geçici** yeniden gönderim toplama penceresi; bir kayıt kalırsa count=1, 2..5 gelirse tek toplu ACK |
| Normal custody istekleri kısa aralıkta geldi | 5 kalıcı kayıt olunca hemen 1 toplu ACK; 1..4 için ilk kayıt zamanından itibaren geçici en çok 120 saniye |
| **Güvenilir** seyrek raporlama profili (ör. ≥60 dakika) biliniyor | `trusted_sparse_single_ack=true` seçilirse, ilk kalıcı custody kaydı için hemen count=1 |
| Yeni PERIODIC LIVE | ACKless, en yeni konum önce; hiçbir 0x0A/0x09, 5'li cache veya geçmiş eksikliği LIVE TX'i kilitlemez |
| Deneysel PERIODIC HISTORY custody (birleşik LAB hedefi) | OBS1'e aynı exact ciphertext Flash'ta kalıcı tutulduktan sonra Gateway'e custody bit'i açık gider; uygun 0x0A ACK kapsamına alınır; kritik EVENT için bir exact-cache yuvası korunur |
| Diğer KQ05 LAB imajı | Eski ACKless HISTORY / Backend 0x09 deneysel rotası korunur; yeni yerel HISTORY devri sadece birleşik bayrakla opt-in |

**Önemli:** Gateway henüz Tracker'ın güvenilir raporlama periyodunu almıyor. Seyrek tekli seçenek host seviyesinde parametrelenmiş ve testlenmek üzere eklenmiştir; gerçek Gateway başlangıcında otomatik açık **DEĞİL**. Paket başlığı / dışarıdan gelen sözde periyot bu politika için güven kaynağı değildir.

**5 yalnız ACK içindeki maksimum referans sayısıdır; yeni LIVE TX veya Gateway noncustody kabul limiti değildir.** RF TX engeli varken OCQ2'deki ACK tutacakları korunur; veri yalnız gerçek kayıt yetkisiyle dayanıklı okunduktan sonra onaylanır. `Radio.Send()` çağrısı Tracker alımı değildir. Uzun süre internet/LoRa kesintisinde Flash kapasitesi sınırlıdır; sınırsız veri garantisi yoktur.

## PC doğrulama

```bash
bash tools/ci/run_kq05_one_ack_host.sh
bash tools/ci/compare_kq05_ack_image_sizes.sh
```

İlk komut kaynak sözleşmesi ve host regresyonlarını çalıştırır. İkinci komut iki **ayrı deneysel** RAK imajını `pio run` ile derler; **PlatformIO RAM/Flash linker raporlarından** kullanım ve farkları gösterir. NRF52 hedefine göre `firmware.bin` oluşması zorunlu değildir ve karşılaştırma buna bağlı değildir. **Cihaza upload, Flash erase, servis/DB müdahalesi yoktur.** Fark, iki imajın tüm özelliklerinin farkıdır; yalnız 0x05 nesnesini çıkarmanın tek başına kaç bayt kazandırdığını izole etmez. Herhangi bir başarısız test veya derleme = BLOCKED.

### 19 Kasım 2026 PC çalıştırma gözlemi (PR #355)

- `run_kq05_one_ack_host.sh`: **PASS** (PC model / sahada RF doğrulaması değil).
- `pio run -e rak4630_kq05_lab`: **SUCCESS**, Flash **475.012 / 815.104 B** (%58,3), RAM **51.456 / 248.832 B** (%20,7).
- `pio run -e rak4630_kq05_unified_ack_lab`: **SUCCESS**, Flash **482.812 / 815.104 B** (%59,2), RAM **51.820 / 248.832 B** (%20,8).
- Birleşik ACK imajı bu iki LAB hedefinin karşılaştırmasında **Flash +7.800 B**, **RAM +364 B** kullanıyor. **Bu aşamada firmware küçülmemiştir.** Farklı deneysel özelliklerin toplu maliyeti olduğundan yalnız ACK kodunun marjinal maliyeti olarak sunulmaz.
- İki başarılı derlemeden sonraki *eski* betik `firmware.bin` beklediği için `DUR: Eksik firmware image` demiştir; derleme başarısızlığı değildir. Karşılaştırma betiği PlatformIO bellek özetine geçirildi ve sonraki kullanıcı PC çıktısında PASS alındı. Yukarıdaki bellek rakamları önceki HEAD içindir; yeni HISTORY kodu tekrar ölçülmelidir.



## 19 Kasım — Tasma → Gateway HISTORY custody devamı

Ürün kararı: **Normal HISTORY'de de Gateway yerel sorumluluğu üstlenecek.
Tasma Gateway ACK'si için Edge/Backend beklemeyecek.** Gelen şifreli
paket Gateway OCQ2 Flash'ına kalıcı yazılmalı ve scoped HMAC yetkisiyle
`0x0A` tekli/toplu (count 1..5) custody ACK üretilmelidir.

Birleşik LAB hedefinde, kritik EVENT için mevcut exact-cache-before-RF
göndericisine opt-in PERIODIC HISTORY modu eklendi. Bu sınıf aynı
`ProductRecordStore`, `ProductSecureCrypto` ve `RadioManager`
yetkilerini kullanır; ikinci ACK wire protokolü yoktur.
HISTORY her şifreli paketi **önce OBS1 exact cache'e yazıp** aynı baytları
tekrar gönderir; kayıp makbuzda yeni nonce üretmez.

HISTORY için 60s deneysel pacing, yuvarlanan cached-exact retry,
8 kayıt/loop tarama ve kritik EVENT'e **1 cache yuvası rezervi**
kullanılıyor. En çok 4 PERIODIC cache + 1 EVENT aynı anda var olabilir;
bu **LIVE'ın maksimum 4/5 gönderim penceresi değildir**. Yeni LIVE
ACKless ve cache'den bağımsız kalır; 5/5 dolulukta da gönderim yolu açıktır.
Kritik EVENT → LIVE → HISTORY radyo önceliği korunur.
Geçici Flash busy/radio duty zamanlaması fiziksel test ister.

Yanlış yönde eklenmiş fakat üretim servisine bağlı olmayan adaptive
Backend 0x09 grup seçicisi bu daldan geri alındı. Var olan bağımsız
Backend doğrulama ve makbuz kaynakları değiştirilmedi.

Bu yeni dilimin **host regresyonu ve RAK4630 firmware derlemesi henüz PC'de
PASS edilmedi**. Sonraki komutlar:

```bash
bash tools/ci/run_kq05_one_ack_host.sh
bash tools/ci/compare_kq05_ack_image_sizes.sh
```

Gateway→Edge→Backend kalıcı handoff/reclaim, gerçek yetkilendirilmiş
LoRa alım pencereleri, çoklu tasma fairness, RF bütçe ve güç ölçümleri
hâlâ saha kapısıdır. NO UPLOAD / NO ERASE / NO MERGE.

## Henüz olmayan ürün kabulü

- Gateway'in güvenilir tasma-periyot profiline bağlanması; 60 dk modunda doğru dinleme/ACK penceresi
- Normal PERIODIC HISTORY custody için ilk gerçek deneysel Tracker gönderici bağlantısı `TrackerCriticalEventTx::Mode::kPeriodicHistory` ile eklendi. Aynı exact encrypted paket Flash'ta saklanır; Gateway kendi kalıcı OCQ2 deliline dayanarak 0x0A üretir; Tracker ancak grant/HMAC/exact-byte kanıtıyla OBS1 release eder. **Yeni HEAD'in test ve RAK derleme doğrulaması henüz alınmadı; saha onayı değildir.**
- 0x09 Backend zinciri ve tek yetkili kayıt serbest bırakma politikası, Edge/Gateway kalıcı veri saklama sınırları
- Gerçek SX126x 1/2/5 ACK on-air, zayıf bağlantı/duty budget, çoklu Tracker fairness, restart/revocation ve mA ölçümü
- Kanıtlanmış ürün derlemesi / #221 P0 saha kabulü

**DEFAULT / MAIN / CİHAZLAR KORUNUR. PR TASLAKTIR.**
