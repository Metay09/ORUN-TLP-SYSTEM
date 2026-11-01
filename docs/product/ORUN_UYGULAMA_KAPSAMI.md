# ORUN v1 — kapsam ve sıra

**Durum:** Sahibi onayladı, 1 Kasım 2026. Önceki 150 maddelik taslak kapsamın yerine geçer.
Bu listede olmayan iş v1 değildir; yapılmaz, önerilmez, "ileride lazım olur" diye hazırlığı yapılmaz.

## Çalışma kuralı

1. Her adım **tek PR** olur ve **aynı gün main'e girer** ya da kapatılır. Birleşmemiş dalın üstüne yeni dal açılmaz.
2. Her adım, sahibin **uygulamada ya da cihazda göreceği** bir değişiklikle biter. "Hazırlık", "preflight", "shadow", "prova" adımı açılmaz.
3. Test, değişen davranış için yazılır ve son hal için bir kez koşulur. Aynı kodu tekrar tekrar doğrulamak ilerleme sayılmaz.
4. Bir adım bitince bu belgedeki durumu güncellenir. Yeni mimari belgesi açılmaz.

## Uygulama ilkeleri

- Uygulama yalnız gerçekten çalışanı gösterir. "Planlı", "demo" etiketi yok.
- Uydurma bilgi yok. Her değerin kaynağı var.
- "Şimdi nerede" = ölçüm zamanı en yeni kayıt. Bayat konum asla canlı gösterilmez.
- İnternetsiz çalışır, internet gelince kendiliğinden eşitler. Kullanıcı eşitleme için düğmeye basmak zorunda kalmaz.
- Sade Türkçe. Teknik ayrıntı yalnız "Teknik ayrıntılar" altında.
- Tek gateway: sabit/gezer yalnızca kurulum bilgisidir, ayrı mod ya da yazılım değildir.

## v1 sırası

| # | Adım | Sahibin göreceği | Durum |
|---|---|---|---|
| 0 | Toparlama | main = çalışan sistem, tek dal, bu liste repoda | tamam (#228) |
| 1 | Otomatik eşitleme | Uygulamayı açınca her tasmanın son konumu kendiliğinden gelir; geçmiş arka planda tamamlanır | tamam (#229) |
| 1a | Tek sunucu | PC'de üç servis yerine tek `orun-sunucu`; PC Edge aynı adrese yükler | tamam (#230, #231) |
| 1b | Kalıcı giriş | Kullanıcı adı ya da e-postayla giriş; uygulama kapanıp açılınca oturum açık | bu PR; PC'de `bash tools/orun_guncelle.sh` |
| 1c | Cihazı hesaba ekle | Tasmayı USB/BLE ile bağla → "Hesabına ekle"; PC aracı yok | — |
| 1d | Gateway çiftlik anahtarıyla | Gateway bir kez eklenir; yeni tasma için gateway'e dokunulmaz | — |
| 2 | İnternetsiz harita | İnternet yokken uydu ve harita (Aydın–Muğla, mera çevresi yakın zoom) | temel kod var |
| 3 | USB'den tüm ayarlar | Kabloyla her ayar değişir | rapor aralığı, hizmetler, alanlar var |
| 4 | BLE'den tüm ayarlar | Aynısı kablosuz, alanlar dahil | yalnız okuma var |
| 5 | Alan dışı bildirimi + LOST | İnek alandan çıkınca telefona bildirim; kayıp durumu | tasma alanı kendisi kontrol ediyor |
| 6 | LoRa'dan tüm ayarlar | Uzaktan ayar; "sırada → ulaştı → uygulandı" durumu | yok |
| 7 | Gateway'in gözetimsiz çalışması | Eski telefon dokunulmadan 7/24 çalışır, yeniden başlar, yeniden bağlanır | ön plan servisi var |
| 8 | Haberleşme sağlığı | Haritada bölge bazlı sinyal ve paket başarısı | temel var |
| 9 | Güneş paneli katkısı | Pil ekranında şarj durumu ve güneş katkısı | yok |

### Ayarlanabilecekler (3, 4, 6)

Rapor aralığı, alan dışı aralığı, alanlar ve hangilerinin açık olduğu, hizmetler (konum/röle/gateway), LOST ayarları, serbest otlatma, cihaz adı, BLE'yi açma, telsiz ayarları (frekans, SF, güç).

- Üç kapı (USB, BLE, LoRa) cihazdaki **aynı** ayar sahibini kullanır; üç ayrı ayar sistemi yazılmaz.
- LoRa'dan gelen ayar tasmanın bir sonraki raporunda uygulanır.
- Telsiz ayarı uzaktan değişip onay gelmezse cihaz eski ayara kendisi döner.

### Mimari (sahip kararı 2026-11-01)

Tasma → Gateway (internetli telefon ya da Linux köprüsü) → **tek ORUN sunucusu** → uygulama.
Cihazlar birbirine değil **hesaba (çiftliğe)** eklenir; gateway ve röle yalnız taşır.
Gateway tasma listesi/imzalı izin taşımaz, çiftlik anahtarıyla çalışır (1d).

## v1 sonrası

DIY sabit gateway, sensör (nem/sıcaklık, tank), vana/pompa, kişi/araç takibi, mesajlaşma, yardımcı kullanıcılar, hayvan defteri/fotoğraf, PDF rapor, yer işaretleri, uygulama kilidi.
