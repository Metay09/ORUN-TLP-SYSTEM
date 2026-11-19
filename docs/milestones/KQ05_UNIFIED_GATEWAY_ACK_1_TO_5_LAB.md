# KQ05 — Tek Gateway ACK motoru, 1..5 kayıt (LAB / taslak)

**Tarih:** 19 Kasım 2026. **Durum:** DRAFT, gerçek RAK/LoRa doğrulaması YOK.
Başlangıç: PR #354. Bu geliştirme yalnız deneysel `ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF` altında çalışır; varsayılan `rak4630` ve `rak4630_kq05_lab` hedefleri değiştirilmez.

## Karar

Aynı Gateway custody ACK protokolü **0x0A** hem **tek kaydı (count=1, 56B)** hem **2..5 kaydı (64..88B)** imzalar/doğrular. İki farklı Gateway onay motoru bir arada çalıştırılmaz:

- Deneysel Gateway: tek `Kq05GatewayBatchCollector` + `Kq05GatewayBatchRuntime` + tek `RadioManager::sendAuthorizedKq05GatewayBatchAck`. Eski 0x05 Gateway custody runtime'ı bu derlemede kurulmaz.
- Deneysel Tracker: tek `Kq05TrackerBatchRadioRuntime` + `Kq05TrackerBatchReleaseOwner`. Eski `CustodyAckReleaseOwner` 0x05 alıcısı, RX / Flash service ve global nesnesi bu imajda **derleme dışında**. Default firmware'de eski 0x05 değişmeden kalır.
- Tekli ve toplu 0x0A aynı yetkilendirilmiş grant, tam SHA256, kapsamlı HMAC, exact Flash okuma, tek tek dayanıklı OBS1 release-marker yolundan geçer. **56B count=1 bile HMAC/doğrulama olmadan kayıt silmez.**
- **0x09 Backend-DURABLE makbuzu ayrı bir güvenlik sınırıdır.** Normal HISTORY için Gateway'in OCQ2'ye dayalı yerel custody ACK'si esastır. Birleşik 0x0A LAB imajı ikinci OBS1 release sahibi olan 0x09 alıcısını **derleme dışında** tutar; ayrı KQ05 LAB imajının 0x09 yolu korunur.

## Uyarlanabilir politika (deneysel)

| Durum | Beklenen 0x0A |
| --- | --- |
| Kritik EVENT, Gateway OCQ2 Flash'a kalıcı yazdı | Hemen tekli count=1 flush (önce bekleyen farklı grubu flush et, olayı tekrar iste) |
| Önceden Gateway'e yazılmış exact kayıt ACK kaybıyla tekrar geldi | Deneysel varsayılan **3 saniye** tekrar toplama penceresi; bir kayıt kalırsa count=1, 2..5 gelirse toplu ACK |
| Normal custody istekleri kısa aralıkta geldi | 5 kalıcı kayıt olunca hemen 1 toplu ACK; 1..4 için ilk kalıcı kayıt zamanından itibaren varsayılan **3 saniye** |
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

Bu HISTORY dilimi `e7c2b84e50dd5e2e51892c500ae9c79c91810c09`
üzerinde operatör PC'sinde host/fake-NOR ve iki RAK4630 imaj derlemesinden
**PASS** aldı: normal LAB Flash **475.652 B**, RAM **51.464 B**;
birleşik LAB Flash **482.804 B**, RAM **52.012 B**. Delta **+7.152 B
Flash / +548 B RAM** (ayrı imajların toplam farkıdır, izole ACK maliyeti
olarak sunulmaz). Bundan **sonraki sadeleştirme commit'i** henüz
operatör PC'sinde test edilmedi. Sonraki komutlar:

```bash
bash tools/ci/run_kq05_one_ack_host.sh
bash tools/ci/compare_kq05_ack_image_sizes.sh
```

Gateway→Edge→Backend kalıcı handoff/reclaim, gerçek yetkilendirilmiş
LoRa alım pencereleri, çoklu tasma fairness, RF bütçe ve güç ölçümleri
hâlâ saha kapısıdır. NO UPLOAD / NO ERASE / NO MERGE.

## Sadeleştirme kararı — 19 Kasım 2026

**Ürün için tek kalıcı sorumluluk zinciri korunur:** `OBS1` (Tasma)
→ `ProductCustodyStore / OCQ2` (Gateway)
→ `Sf5l0EdgeHandoff` (Edge teslimi ve Gateway reclaim)
→ mevcut PC Edge/Android köprüsü → mevcut Backend verified ledger.
Yeni bir OCQ2, Bridge SQLite uploader'ı veya Backend deposu yazılmaz.

**Derleme başına tek Tracker kayıt bırakma sahibi:**

| Derleme | Custody / PERIODIC release sahibi |
| --- | --- |
| `rak4630_orun` | Eski yetkili `0x05` sahibi (davranış değiştirilmedi) |
| `rak4630_kq05_lab` | Eski `0x05` ve ayrı Backend `0x09` yetkileri (eski LAB uyumluluğu; fiziksel çıkış değil) |
| `rak4630_kq05_unified_ack_lab` | **Yalnız Gateway `0x0A`**. Backend `0x09` RX mailbox, servis ve global release nesnesi derleme dışında. |

İlk iki LAB yolu, kullanıcıya yeni bir ürün modu olarak sunulmaz.
Yeni tekli/toplu `0x0A` için ayrı wire verifier gerekebilir; fakat
aynı exact Flash, SecurityStore, OCQ2, Edge handoff veya Backend
implementasyonunun kopyalanması kabul edilmez.

**Birleştirme ENGELİ:** `main@3df6f512` ile KQ05 HEAD zincirleri
ayrışmıştır. O tarihli GitHub karşılaştırmasında KQ05 deneysel taraf
`main`'den 397 commit ileride, güncel `main` ise 11 ayrı commit ileridedir.
Bu sayılar PR #355'in tek başına büyüklüğü **değildir**; PR #355
kendi tabanına göre 21 dosya içerir. Main'deki Android arka plan
Gateway köprüsü ve diğer ürün düzeltmeleri KQ05 dalında yer almaz.
Doğrudan toplu merge yapılmaz: önce mevcut ürün `rak4630_orun`
için G0 salt okunur cihaz/anahtar/Flash/servis envanteri, sonra
yalnız gerekli değişiklikleri **mevcut sahipleri genişleterek** tek
ürün kaynağına taşıma. Host kanıtı, gerçek LoRa kabulü değildir.

Son sadeleştirme kodu, `0x09`yi yok etmez: yalnız birleşik
`0x0A` imajında yarışan alternatif release yetkisini ve dolaylı
RX durumunu kaldırır. Diğer KQ05 test senaryoları korunur. Cihaza
yükleme / erase / servis / DB / `main` değişikliği yapılmadı.

### 19 Kasım 2026 — 10 saniyelik dinleme penceresi için zamanlama adayı

Kaynak denetimi: `radio_config::kWindowedRxAfterTxMs = 10000`.
Önceki birleşik Gateway varsayılanları `partial_flush=120000` ve
`retry_coalesce=30000` ms idi; tasma 10s RX penceresi ile
**aynı raporlama sırasında ACK alınmasını desteklemiyordu**.
Yeni opt-in birleşik LAB varsayılanı ikisi için **3000ms** yapıldı.
Bu değişiklik yeni protokol veya store oluşturmaz; var olan
`Kq05GatewayBatchRuntime` zamanlayıcısını düzeltir. Önceden kullanılan
uzun süreler açıkça parametrelenen sentetik host senaryolarında hâlâ test
edilir; gerçek product composition yalnız kısa varsayılanı kullanır.

Yeni host testi nominal single 0x0A ACK ve lost-ACK duplicate yeniden
onayının 3s sınırında request edilmesini, exact OCQ2 kaydının yeniden
yazılmamasını kanıtlamayı hedefler. **Bu bir alım garantisi değildir**:
RX penceresi Tracker TX_DONE'dan başlar, Gateway timer ise Flash commit
sonrasında; RF airtime, geceleme/uyku, TX duty/driver ve iki saat
senkronizasyonu donanımda ölçülmelidir. `radio_.canSend()` false ise
ACK ertelenebilir ve kayıtlar güvenle elde tutulur; uygulama sahada
başarı iddia etmez.

## Henüz olmayan ürün kabulü

- Gateway'in güvenilir tasma-periyot profiline bağlanması; 60 dk modunda doğru dinleme/ACK penceresi
- Normal PERIODIC HISTORY custody için `TrackerCriticalEventTx::Mode::kPeriodicHistory` eklendi. OBS1 exact ciphertext saklanır ve `0x0A` kanıtıyla selective release edilir. `e7c2b84` PC PASS; sonraki sadeleştirme HEAD'i bekliyor. Saha onayı yok.
- 0x09 Backend zinciri ve tek yetkili kayıt serbest bırakma politikası, Edge/Gateway kalıcı veri saklama sınırları
- Gerçek SX126x 1/2/5 ACK on-air, zayıf bağlantı/duty budget, çoklu Tracker fairness, restart/revocation ve mA ölçümü
- Kanıtlanmış ürün derlemesi / #221 P0 saha kabulü

**DEFAULT / MAIN / CİHAZLAR KORUNUR. PR TASLAKTIR.**
