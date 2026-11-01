# ORUN S6C — Hesaba bağlı kalıcı ortak veri aynası

**Durum: TASLAK, 30 Ekim 2026. Bu dalın testleri kullanıcının PC'sinde
henüz çalıştırılmadı; PASS iddiası yok.**

**Amaç:** Kullanıcı adı/şifre + önceden yetkili Android cihazıyla başarılı
giriş yapan sahibin, sunucudan gelen `zone/group/zone_target` **taslak**
değişikliklerini Android'in mevcut SQLite `Db` arayüzüyle atomik
saklamasına hazırlanmak. **Tek tablo**, ayrı bir gözlem defteri veya ayrı
bir veritabanı oluşturulmaz. Veri kullanıcı hesap/site/telefon sınırıyla
anahtarlanır.

## Somut uygulama

- `edge/app/src/app/accountSharedMirror.ts`
  - Mevcut `Db` arabirimini alır ve sadece
    `orun_account_shared_mirror_v1` tablosunu oluşturur.
    `orun_edge` gözlem defterinin kayıtlarını, okunmuş balık kayıtlarını,
    cihaz/USB/BLE verilerini veya sahibin mevcut
    `orun.app.zones.v1` / `orun.app.zoneTargets.v1`
    taslaklarını **silmez, dönüştürmez ve değiştirmez**.
  - Yalnız **aktif hesap oturumuyla** snapshot okunur.
    `site_id + actor_id + installation_id` anahtarı
    hesabın veya telefonun eski önbelleğini başka hesaptan saklar.
    Ortak veriler sunucuda aynı siteye ait olduğu hâlde,
    yeni bir telefonda yerel ayna 0'dan yeniden oluşturulur.
  - `OwnerAccountClient.readSharedChanges(cursor)` gerçek hesap bearer'ıyla
    `GET /api/v1/sync` çağırır. Parola, device bearer veya
    account bearer SQLite içine **ASLA yazılmaz**.
  - Mevcut `OfflineSyncStore.applyPull` kendi kuralıyla artan
    `sequence`, `revision`, silinen kayıt ve aynı sürümde
    farklı gövdeyi doğrular. Böylece ikinci bir revizyon
    motoru yazılmamıştır.
  - Sunucudan alınan **bir sayfa**, geçerli önbellekle
    birleştirilip tek SQLite `transaction` içinde
    `state_json + cursor` olarak kaydedilir.
    COMMIT başarısızsa eski kayıtlar ve cursor korunur.
    Yazmadan önce ve sonra kullanıcı/telefon oturumu doğrulanır.
    5 sayfalık bounded pull, toplam 2 MiB bellek aynası limiti;
    bozuk yerel JSON durumunda kayıt korunarak işlem reddedilir.
  - `snapshot()` sadece sunucuda kanonik kabul edilen alan,
    grup ve hedef taslaklarını verir. **Tasma `APPLIED`
    veya geofence komutu gönderildi sonucu ASLA üretilmez.**
  - **Bu dal henüz Android ekranına ve Controller'ın
    aktif SQLite bağlantısına BAĞLANMADI.**
    Canlı bağlantıyı doğru `core.exclusive` kilidi ve
    Android yedekleme/at-rest gizlilik sınırı doğrulanmadan
    açmayacağız.

- `edge/app/src/app/ownerAccountClient.ts`: hesabın kısa JSON login
  yanıtına 8 KiB kısıt korunarak, gerçek `/sync` sayfası için
  ayrı **512 KiB** sınırlı JSON yanıt doğrulaması. Önceki
  küçük sınır gerçek mera sayfalarının gereksiz reddine yol
  açabilirdi.

## Host kabul

```bash
bash tools/ci/run_account_sqlite_mirror_host.sh
```

`node:sqlite` (gerçek in-memory SQLite) ile şu durumlar:
- İki sayfalık sunucu değişikliği atomik SQLite kayıt ve restart sonrası
  aynı cursor/alan/grup aynası
- Farklı site/actor/telefonun başka hesabın aynasını görmemesi
- Aynı revizyonda farklı gövdenin reddi
- Simüle edilen COMMIT hatasında cursor rollback ve sonraki tekrar
- Kırık JSON aynasının kaybedilmeden bloklanması
- Kullanıcı çıkış yaparken HTTP devam ediyorsa kaydetmeme
- Önceki `offlineSync.test.ts` testlerinin tekrarı
- Önceki S6A/S6B TypeScript oturum kontrolleri ve `tsc --noEmit`

**Bu testler henüz çalıştırılmadı.** PASS sonucu yalnız
gerçek kullanıcı terminal çıktısı ile belirlenecek.

## Dağıtım / ürün sınırı

- Gerçek ORUN servisleri `orun-backend.service`,
  `orun-pc-edge.service`, `orun-shared-sync.service`,
  `orun-shared-pc-mirror.timer` ve Tailscale 443/8443
  **değiştirilmedi**.
- `main` / gerçek APK / firmware **değiştirilmedi**;
  yeni testler sadece SQLite bellek veritabanı ve mock HTTP.
- Mevcut ORUN Android SQLite `orun_edge` gözlem defteri
  şifreleme olmadan oluşturuluyor. Paylaşılan mera koordinatları
  da gizlilik taşıdığından, **Android backup kuralları,
  cihazdaki şifreleme, ekran erişim kontrolü ve veri kaybı
  senaryoları üretim geçişinden önce incelenmeli**.
- Offline veri telefon belleğinde saklanabilse de, S6B hesabı
  uygulama yeniden açıldığında tekrar giriş gerektiriyor.
  **İnternet yokken oturum açıp bu aynayı görüntüleme**
  deneyimi henüz hazır değil. Nihai offline-first hedef
  için ayrıca güvenli cihaz-içi yeniden doğrulama gerekiyor.
- **Outbound edit/outbox**, çakışmaların sahibi tarafından
  çözülmesi, otomatik 2 yönlü eşitleme, merkezî hayvan/tasma
  kimlikleri, son gerçek konum ve geçmiş kayıtları
  daha sonra ayrı denetimlerden geçecek.
