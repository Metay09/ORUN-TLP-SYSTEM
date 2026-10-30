# ORUN S5A — Owner-approved phone registration over HTTPS (candidate)

**Durum: DRAFT, kod yazıldı; kullanıcı bilgisayarında HOST PASS ve fiziksel
Android imza testi henüz yapılmadı. Canlı ORUN Backend'i ve APK DEĞİŞMEDİ.**

## Mevcut somut başlangıç

30 Ekim 2026: kullanıcının gerçek Debian PC'sinde
`orun-backend.service` (gözlem), `orun-pc-edge.service`,
`orun-shared-sync.service`, `orun-shared-pc-mirror.timer` aktif.
Çalışan ortak veri Backend'i sadece `127.0.0.1:18781` ve
Tailscale Serve HTTPS `https://simsek.tail823510.ts.net:8443` üzerinden
tailnet içinden erişilebilir. Android Chrome'da yetkisiz
`GET /api/v1/sync/identity` → HTTP 401 doğrulandı.
Kalıcı `~/ORUN-SHARED-RUNTIME` kaynakları **pinned snapshot**
`264b4117a4d9` ve bu geliştirme dalıyla otomatik güncellenmez.

Bu dal **kodu değiştirir, fakat çalışan snapshot'a dokunmaz.**
Canlı `8443` arkasındaki mevcut Backend'de telefon kayıt
`/api/v1/enroll/begin` ve `/api/v1/enroll/finish` henüz **açık değil**.
Pilot testte bu endpoint'leri aktifmiş gibi göstermeyin.

## S5A yeni bileşenler

1. `backend.edge_enrollment` mevcut Ed25519 PC anahtarlarını ve
   30-günlük tokenlarını koruyarak **P-256 SHA256withECDSA** ekler.
   Mevcut SQLite'ye `key_algorithm DEFAULT 'Ed25519'` eklenir.
   P-256 raw key değil **kanonik X.509 SubjectPublicKeyInfo DER** kullanır;
   algoritma-bağımsız imza değiştirilebilir değildir:
   Ed25519 eski `ORUN:STEP3C:EDGE-ENROLL:v1\0`,
   P-256 `ORUN:STEP3C:EDGE-ENROLL:P256:v2\0`.
   Sunucu nonce ve owner-code kontrolünden sonra bir defalık bearer verir.
2. `backend.phone_enrollment_http`: yalnız seçilirse
   `POST /api/v1/enroll/begin` ve
   `POST /api/v1/enroll/finish` çalışır. Begin isteğinden
   sahip/scopes/rol okunmaz. Kurulum kimliği sunucu tarafında
   `edge-<sha256(SPKI DER)>` şeklinde türetilir.
   Henüz sahibi onaylamadan hiçbir token üretilmez.
   Aktif bekleyen istekler en çok 32, 15 dakika ömürlüdür;
   approved isteği anonim repeat-begin ile sıfırlanmaz.
   Varsayılan `tools/orun_shared_sync_loopback.py` çağrısında bu
   endpoint'ler KAPALIDIR; yalnız açık
   `--enable-phone-enrollment` bayrağıyla aktif olur.
3. `tools/orun_shared_phone_approve.py`: yalnız sahibi tarafından
   terminalde kullanılacak açık onay komutu.
   Telefon ekranındaki **istek ID + anahtar parmak izi** ile
   bilgisayardaki girilen bilgilerin eşleşmesi zorunludur.
   `--approve-verified-phone` olmadan reddeder.
   `read-only` ve `editor` sadece
   mera/grup/hedef taslak okuma-yazma izinleridir;
   Gateway/Tracker/APPLIED yetkisi değildir.
   İşlem sonucundaki bir defalık gizli kod **asla destek sohbetine,
   GitHub'a veya loglara gönderilmeyecek**, yalnız telefona girilecek.
4. `OrunMobileIdentityPlugin.java`, `orunMobileIdentity.ts`:
   AndroidKeyStore P-256 anahtar çiftini ancak açık kullanıcı eylemiyle
   oluşturur. Gizli anahtar cihazdan çıkmaz; genel amaçlı imza
   verilmez, yalnız sunucunun domain-separated 32-byte challenge'ı
   imzalanabilir. Android Keystore EC/P-256 API, Android'in resmi
   belgelerinde yer alır; eski yalnız-Ed25519 OEM varsayımı yerine
   P-256 uyumluluk yolu hazırlanmıştır. Bu değişiklik kullanıcının
   Android cihazında **henüz fiziksel PASS değildir**.
5. `edge/app/src/app/phoneEnrollment.ts`: HTTPS dışındaki uzak
   bağlantıları reddeden açık `begin/finish` taşıyıcısı.
   Sunucu dönen kimliği ve isteği Android kimliğiyle eşleştirir.
   **Henüz uygulamanın ekranlarına bağlı değildir.** Cevapta gelen
   tokenı kalıcı saklamaz; native korumalı credential store
   uygulanmadan gerçek telefonda tamamla çağrısı kullanılmamalıdır.
   Gözlem Backend tokenı yeniden kullanılmaz.

## Doğrulama

```bash
bash tools/ci/run_phone_owner_enrollment_host.sh
```

Geçici SQLite ve `127.0.0.1:0` HTTP süreçleri;
mevcut EdgeEnrollment/PC enrollment/loopback testleri, P256+Ed25519
gerçek HTTP begin/finish, imza ve yanlış proof reddi, fingerprint,
read-only/owner yetkisi, revoke, 32 pending sınırı, legacy SQLite
uyumluluğu ve TS HTTPS/identity bağlama denetimleri.

**Yukarıdaki testlerin PASS sonucu henüz elde edilmedi.**
Mevcut servisleri durdurmaz, restart etmez, Tailscale ayarını değiştirmez;
APK derlemez.

## S5B hazırlığı — cihaz içi korumalı oturum kasası (aynı taslak dal)

- `edge/app/android/app/src/main/java/tr/orun/edge/OrunMobileSessionPlugin.java`:
  AndroidKeyStore AES-256-GCM, 12-byte rastgele IV, AAD, atomik dosya
  yazımı, tek thread seri erişimi. Şifreli dosya yalnız Android
  `getNoBackupFilesDir()` içinde; telefon yedeklerine alınmaz.
  `putSession` kimlik anahtarı ile kurulum ID eşleşmesini,
  HTTPS origin/site/token ve 30-gün süre sınırını doğrular.
  `getSession` varsayılan olarak bearer tokenı JS'ye vermez;
  token yalnız açık `includeToken:true` çağrısına verilir;
  bozuk/eski/kopya oturum sessizce kabul edilmez.
  `clearSession` yalnız yerel oturumu siler, **sunucu yetkisini
  iptal etmez**; server revoke ayrıca gerekir.
  `MainActivity` iki native bridge'i kaydeder fakat otomatik
  kimlik oluşturmaz veya sunucuya kayıt isteği göndermez.
- `edge/app/src/platform/orunMobileSession.ts`: türlenmiş
  `storeMobileSession`, `mobileSessionStatus`,
  `activeMobileSession`, `clearLocalMobileSession`.
  Hiçbir token `localStorage`/IndexedDB'e yazılmaz.
- `edge/app/src/app/completePhoneEnrollment.ts`: sunucunun
  `/api/v1/sync/identity` üzerinden bearer'a bağladığı gerçek
  `site + installation` değerlerini doğrular, yalnız sonra
  native AES kasasına yazdırır. İstemciye gizli tokenı geri
  döndürmez. Android app ekranına henüz bağlanmadı.
- `tools/ci/test_complete_phone_enrollment.ts`: eşleşmeyen
  kurulum kimliği, HTTP, yanlış onay, tokenın UI'ye
  döndürülmemesi ve korumalı depo çağrısı için host sınır testleri.
  `run_phone_owner_enrollment_host.sh` içine eklendi.

**S5B henüz Android SDK derleme/gerçek cihaz testi geçmedi.**
Java `OrunMobileSessionPlugin` için gerçek Android compile ve
Keystore AES depolama/geri-açma/expired testleri gerekir.

**Dikkat:** Kayıt sunucuda başarıyla bitip Android'de token
saklanamazsa sunucudaki telefon kurulumu onaylanmış kalabilir.
Bu durumda sahibi kontrollü yeniden anahtarlama/oturum yenileme
akışı olmadan yeniden kaydolmaya çalışmamalı. Bu akış S5C
güvenlik bloklayıcısıdır.

## Takip eden ürün işleri (henüz bitmedi)

- Android **native korumalı bearer saklama** için aday kod var; gerçek
  Android fiziksel doğrulama, süresi dolan yetkiyi güvenli yenileme ve
  cihaz değişikliği/iptal uygulama ekranları hâlâ gerekiyor.
- Uygulama içinde ilk kurulum, güvenli kullanıcı **hesabı/oturum açma**,
  sahip onayı, yardımcı rolü ve çok-telefon yönetimi.
  Yerel sahip-onay CLI, kalıcı kullanıcı hesabıyla login değildir.
- HTTPS üzerinden kayıt için Tailscale dışındaki üretim altyapısı;
  request rate limiting ve abuse kontrolü; sunucu yedekleme ve
  göç/rollback stratejisi; üretim security review.
- Android SQLite outbox/mirror ile gerçek Alanlar ve Sürü ekranları;
  meraların taşınması ile gerçek tasmanın geofence `APPLIED`
  sonucu birbirinden ayrı tutulacak.
- Mevcut çalışan snapshot'ın **ayrı, rollback kontrollü**
  dağıtım onayı; bu PR doğrudan canlı 18781'i güncellemeyecek.

**Harici kaynak yönü:** Android'in resmi Android Keystore EC
örneklerinden desteklenen algoritma ve API davranışı doğrulandı.
Dış projelerden lisans belirsiz kod kopyalanmadı.
