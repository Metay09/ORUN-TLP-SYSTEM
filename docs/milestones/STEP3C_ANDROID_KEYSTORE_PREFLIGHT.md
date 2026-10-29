# ORUN Step 3C Android — cihaz kimliği için Keystore ön kontrolü

**Durum: DRAFT — Android APK/gerçek telefon testleri henüz yapılmadı.**
**Bağımlılık:** S3C PR #198 gerçek HTTP host PASS, 3C Backend PR #195 host PASS.
**Canlı davranış:** Bu PR telefonun ORUN sunucusuna gerçek kaydını veya otomatik alan eşitlemesini BAŞLATMAZ.

## İşlev

- Yeni `OrunIdentityPlugin` yalnız Android'de çalışır.
- Ayarlar > **Bu telefonun kimliği (ön kontrol)** ekranı, var olan anahtarın
  durumunu salt-okunur inceler. Otomatik anahtar oluşturma **yok**.
- Kullanıcı **Güvenli telefon kimliğini hazırla** düğmesine basınca, destekleyen
  cihazlarda `AndroidKeyStore` provider'ına açıkça sabit ad altında Ed25519
  anahtarı oluşturma isteği gönderilir; başarısız olursa **fail closed**.
  Yazılımda tutulan/export edilen bir private-key fallback yoktur.
- 32B ham Ed25519 açık anahtarı, 20 haneli public-key fingerprint'i ve
  `edge-<sha256(public_key)>` installationId'si alınır.
  *Private key*, ORUN JavaScript ya da localStorage'ına gönderilmez.
- İleride enrollment başladığında, yalnız sunucunun `requestId` (16B) ve
  `challenge` (32B) değerine `ORUN:STEP3C:EDGE-ENROLL:v1\x00` domain'ini
  ekleyip **gerçek Ed25519 challenge possession** imzasını çıkarmak için
  `signEnrollment` sınırlı bir köprü olarak eklendi. Arbitrary-message
  signing metodu yok. Bu yöntem UI üzerinden henüz kullanılmıyor.
- Anahtar durumu ekranda açıkça **Henüz sunucuya kayıtlı değil** yazar;
  kayıt onayı/token/Tracker komutu/uygulanmış alan yanılsaması yaratmaz.

## Android uyumluluk uyarısı

- Kaynakta `KeyPairGenerator.getInstance("Ed25519", "AndroidKeyStore")`,
  `KeyGenParameterSpec`, `ECGenParameterSpec("Ed25519")` ile dar kapsam
  istenir. Android 13+ (API33+) denemesi için; OEM/KeyMint/TEE desteği gerçek
  cihazda farklı olabilir. Cihaz desteklemiyorsa kullanıcıya hata gösterilir
  ve mevcut app verileri korunur.
- Kaynak kod ve `npm test` fiziksel Keystore desteği kanıtı değildir.
  Gerçek cihazda anahtar oluşturma, uygulama yeniden açma sonrası aynı
  fingerprint, plugin derleme ve challenge imza round-trip ayrıca yapılmalıdır.
- Desteklemediği cihazlarda güvenliği gevşeten yazılım anahtar yedeği
  eklenmemiştir; farklı güvenli yedek yol için ayrı güvenlik kararı gerekir.

## İlk kullanıcı host testi (2026-10-29)

- Kullanıcının Debian checkout'u `5b0e88e`: `npm test` **95 test / 94 PASS / 1 FAIL**. Hata: `test/orunIdentity.test.ts` İngilizce test adındaki başarısız doğrulama, `/kimlik|Kimlik|yanıt/` regex'inin Türkçe `kimliği` hata mesajını yakalayamaması.
- Güvenlik/kimlik oluşturma uygulama kodu değişmeden, kırılgan metin bağımlılığı kaldırıldı: `fa06c87` test `assert.throws(..., Error)` ile bozuk kimliğin gerçekten reddedilmesini denetliyor.
- Bu düzeltme **henüz kullanıcı makinesinde yeniden test edilmedi**. İlk komut `set -e` ile `npm test`'te durduğu için TypeScript, Android sync ve Gradle derlemesi de **henüz çalıştırılmadı**; APK paylaşımı başlamadı.

## İkinci kullanıcı doğrulaması (2026-10-29)

- Güncel checkout `e356c5d`, test regex düzeltmesi doğrulandı.
- `npm test`: **95/95 PASS, 0 FAIL** (4589 ms); önceki 94/95 test engeli kapandı.
- `npm run typecheck` başlatıldı; paylaşılan log `tsc --noEmit` satırında kesildiği için **PASS henüz kanıtlanmadı**.
- `npm run android`, Gradle debug APK ve fiziksel telefon testi hakkında bu çıktıda **henüz sonuç yok**.
- Test başarısı gerçek cihazda Ed25519 KeyStore destek kanıtı değildir.

## Testler ve kabul

1. `cd edge/app && npm test && npm run typecheck && npm run build`.
2. `cd edge/app && npm run android && cd android && ./gradlew --no-daemon assembleDebug`.
3. **Sadece uygulama üzerine güncelleme:** Önceki ORUN uygulamasını silme,
   Clear data/Cache veya çalışma servislerini durdurma yok. Debug imza
   uyuşmazlığında kurulu uygulamayı koru, yeni anahtar/kurulum stratejisi
   belirlenmeden kaldırma.
4. Telefonun mevcut Alanlar ve grupları korunmuş olmalı.
5. Ayarlar'da **ilk açılışta kimlik hazır değil**, açık kullanıcı eylemi
   sonrasında destek varsa anahtar hazır/iz görünmeli. Yeniden başlatıp aynı
   fingerprint tekrar görülmeli.
6. Keystore unsupported durumunda anahtar yazıldı ya da server enrolled
   iddiası olmamalı. Kök anahtar/gizli veri/log üretimi yok.

**Önemli:** E2E Node host testi PASS olsa da Android için henüz
`begin/approve/finish` HTTP oturum UX, native token koruma/rotasyon,
TLS + Tailscale server, outbox migrasyonu, gerçek iki telefon senkronizasyonu,
fiziksel Tracker geofence USB/BLE/LoRa aktarımı yok. Bunlar sonraki
kontrollü dilimlerdir.
