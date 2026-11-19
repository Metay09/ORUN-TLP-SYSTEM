# KQ05 — Tek Gateway ACK motoru, 1..5 kayıt (LAB / taslak)

**Tarih:** 19 Kasım 2026. **Durum:** DRAFT, gerçek RAK/LoRa doğrulaması YOK.
Başlangıç: PR #354. Bu geliştirme yalnız deneysel `ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF` altında çalışır; varsayılan `rak4630` ve `rak4630_kq05_lab` hedefleri değiştirilmez.

## Karar

Aynı Gateway custody ACK protokolü **0x0A** hem **tek kaydı (count=1, 56B)** hem **2..5 kaydı (64..88B)** imzalar/doğrular. İki farklı Gateway onay motoru bir arada çalıştırılmaz:

- Deneysel Gateway: tek `Kq05GatewayBatchCollector` + `Kq05GatewayBatchRuntime` + tek `RadioManager::sendAuthorizedKq05GatewayBatchAck`. Eski 0x05 Gateway custody runtime'ı bu derlemede kurulmaz.
- Deneysel Tracker: tek `Kq05TrackerBatchRadioRuntime` + `Kq05TrackerBatchReleaseOwner`. Eski `CustodyAckReleaseOwner` 0x05 alıcısı, RX / Flash service ve global nesnesi bu imajda **derleme dışında**. Default firmware'de eski 0x05 değişmeden kalır.
- Tekli ve toplu 0x0A aynı yetkilendirilmiş grant, tam SHA256, kapsamlı HMAC, exact Flash okuma, tek tek dayanıklı OBS1 release-marker yolundan geçer. **56B count=1 bile HMAC/doğrulama olmadan kayıt silmez.**
- **0x09 Backend-DURABLE makbuzu farklı bir güvenlik sınırıdır; bunun Gateway custody ile tek bir silme yetkisine dönüştürülmesi güvenli değildir.** 0x09 ayrı kalır; normal PERIODIC LIVE/HISTORY için uçtan uca Backend makbuz taşıması henüz tamamlanmadı.

## Uyarlanabilir politika (deneysel)

| Durum | Beklenen 0x0A |
| --- | --- |
| Kritik EVENT, Gateway OCQ2 Flash'a kalıcı yazdı | Hemen tekli count=1 flush (önce bekleyen farklı grubu flush et, olayı tekrar iste) |
| Önceden Gateway'e yazılmış exact kayıt ACK kaybıyla tekrar geldi | 30 saniyelik **geçici** yeniden gönderim toplama penceresi; bir kayıt kalırsa count=1, 2..5 gelirse tek toplu ACK |
| Normal custody istekleri kısa aralıkta geldi | 5 kalıcı kayıt olunca hemen 1 toplu ACK; 1..4 için ilk kayıt zamanından itibaren geçici en çok 120 saniye |
| **Güvenilir** seyrek raporlama profili (ör. ≥60 dakika) biliniyor | `trusted_sparse_single_ack=true` seçilirse, ilk kalıcı custody kaydı için hemen count=1 |
| Normal ACKless LIVE/HISTORY | 0x0A onayı **yok**; LIVE önce, HISTORY fırsat buldukça. Üretim Backend 0x09 zinciri tamamlanmadığı için teslim/temizleme garantisi iddia edilmez |

**Önemli:** Gateway henüz Tracker'ın güvenilir raporlama periyodunu almıyor. Seyrek tekli seçenek host seviyesinde parametrelenmiş ve testlenmek üzere eklenmiştir; gerçek Gateway başlangıcında otomatik açık **DEĞİL**. Paket başlığı / dışarıdan gelen sözde periyot bu politika için güven kaynağı değildir.

**5 yalnız ACK içindeki maksimum referans sayısıdır; yeni LIVE TX veya Gateway noncustody kabul limiti değildir.** RF TX engeli varken OCQ2'deki ACK tutacakları korunur; veri yalnız gerçek kayıt yetkisiyle dayanıklı okunduktan sonra onaylanır. `Radio.Send()` çağrısı Tracker alımı değildir. Uzun süre internet/LoRa kesintisinde Flash kapasitesi sınırlıdır; sınırsız veri garantisi yoktur.

## PC doğrulama

```bash
bash tools/ci/run_kq05_one_ack_host.sh
bash tools/ci/compare_kq05_ack_image_sizes.sh
```

İlk komut host testlerinin yanında yalnız kaynak/derleme sözleşmesi denetimi yapar. İkinci komut iki **ayrı deneysel** RAK imajını `pio run` ile derler; `.bin` Flash boyutunu ve farkını yazdırır. **Cihaza upload, Flash erase, servis/DB müdahalesi yoktur.** Fark, iki imajın tüm özelliklerinin farkıdır; yalnız 0x05 nesnesini çıkarmanın tek başına kaç bayt kazandırdığını izole etmez. Herhangi bir başarısız test veya derleme = BLOCKED.

## Henüz olmayan ürün kabulü

- Gateway'in güvenilir tasma-periyot profiline bağlanması; 60 dk modunda doğru dinleme/ACK penceresi
- Normal geçmiş PERIODIC kayıtları için Gateway'den grup onayı tasarımı: Gateway şifreli içerikten mantıksal sequence'i okuyamaz, Tracker ise ACKless yeniden TX için exact ciphertext tutmaz. Bu bağlantı **henüz yapılmadı**; kolay/sahte kimlik ACK'si eklenmeyecek
- 0x09 Backend zinciri ve tek yetkili kayıt serbest bırakma politikası, Edge/Gateway kalıcı veri saklama sınırları
- Gerçek SX126x 1/2/5 ACK on-air, zayıf bağlantı/duty budget, çoklu Tracker fairness, restart/revocation ve mA ölçümü
- Kanıtlanmış ürün derlemesi / #221 P0 saha kabulü

**DEFAULT / MAIN / CİHAZLAR KORUNUR. PR TASLAKTIR.**
