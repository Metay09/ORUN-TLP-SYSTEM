# ORUN S6D — Android Controller SQLite bağlantısı / hesap bazlı indirme

**Durum: 30 Ekim 2026, GITHUB DRAFT, host kabul testi BEKLİYOR.
Canlı sunucu/PC Edge/Tracker/Tailscale/APK değiştirilmedi.**

## Hedef

S6C'de 17/17 Node SQLite ve TS tür kontrolü PASS olan mevcut
`AccountSharedMirror` modülünü, Android'in zaten çalışan
`Controller` → `EdgeCore` → `CapacitorDb("orun_edge")`
bağlantısına **ayrı veritabanı oluşturmadan** bağlamak.

Bu aşamanın tek izinli yolu:
1. Sahibi tarafından kayıtlı P-256 Android telefon
2. S6A/S6B kullanıcı adı + parola ile oturum
3. Ayarlar → Kullanıcı hesabı · pilot → **Mera ve grup taslaklarını
   SQLite'a indir** (açık kullanıcı işlemi)
4. Yalnız `zone/group/zone_target` **server-canonical draft**
   değişikliklerini mevcut SQLite'daki ayrı tablodan göster
5. Çıkış sonrası başka aktörün/site'ın kaydı görüntülenmesin.

## Değişen kod

- `edge/app/src/app/accountSharedMirror.ts`: isteğe bağlı
  `withExclusiveDb(job)` ile sadece SQL okuma, CREATE TABLE ve
  atomic COMMIT'i `EdgeCore.exclusive` altında çalıştırır.
  **HTTP beklenirken kilit tutulmaz**; BLE/USB kayıtları
  kullanıcı sunucudan indirme yanıtı beklerken akabilir.
  Veri değiştiren `transaction` SQL + cursor birlikte commit;
  HTTP sonrası veya DB yazımı sırasında logout olursa rollback.
  Doğrulanmış S6C'nin `OfflineSyncStore.applyPull` kuralları korunur.
- `Controller.start`: core/SQLite açıldıktan sonra,
  `new AccountSharedMirror(db, job => core.exclusive(job))` kaydeder.
  **Sadece kaydetme işlemiyle tablo oluşturulmaz, otomatik indirme
  başlamaz, mevcut upload tokenları/devices/keys değişmez.**
- `ownerAccountPilotRuntime.ts`: açık `pilotAccountDownloadShared`
  ve `pilotAccountStoredSnapshot` için Controller'ın aynı
  `AccountSharedMirror` nesnesini kullanır. Token hâlâ sadece
  `OwnerAccountClient` belleğindedir. Hesap yoksa önbellek okunmaz.
- `AccountLoginPanel.tsx`: açık indirme düğmesi, kalıcı SQLite
  sayacı/cursor/ilk mera taslağı isimleri ve 'Tasmalara uygulanmadı'
  etiketi. Sonraki indirmede kaldığı cursor'dan devam.
  Eski `orun.app.zones.v1`, `orun.app.zoneTargets.v1` yerel
  taslak düzenleyici verilerini değiştirmez.
- `edge/app/test/accountSharedMirror.test.ts`: aynı SQLite
  üzerinde EdgeCore USB session yazımı ve bekleyen HTTP sayfası
  birlikte test edilir. SQLite işi `core.exclusive` kuyruğundan
  geçer ama ağ beklerken kuyruk tutulmaz.
- `tools/ci/test_owner_account_sqlite_wiring.ts`: gerçek in-memory
  SQLite+EdgeCore ile hesap oturum → sqlite hazırlık → authenticated
  pull → imleç/kayıt readback → logout önbellek erişim reddi.
- Kabul: `bash tools/ci/run_account_android_sqlite_wiring_host.sh`.
  Daha önce PASS olmuş 17 S6C testinin yanına yeni concurrency
  ve account runtime testleri, önceki S6A/B TypeScript regresyonu,
  TypeScript strict typecheck.

## Kesinlikle bu aşamada yapılmayanlar

- Canlı dört servis, Tailscale portları, tokenlar, kullanıcı hesapları
  ve telefon APK'sı **değişmedi**.
- `GET /api/v1/sync` sunucuda S6A account-gated login henüz devrede
  olmadığından mevcut telefonda indirme **şu anda çalışmaz**.
- Android'in mevcut `CapacitorDb` bağlantısı
  `createConnection(...,"no-encryption",...)` olarak kuruluyor;
  gizli mera koordinatları için Android backup/şifreleme/cihaz kilidi
  kontrolleri yapılmadan gerçek hesap verisi üretime alınamaz.
  Korumalı account bearer bu SQLite dosyasına KESİNLİKLE yazılmaz.
- Gerçek Android Keystore kayıt/login ve Android native SQLite
  plugin cihaz üzerinde test edilmedi. Şu anki testler
  `node:sqlite` üzerinden çalışacak.
- Kullanıcı sahada internetsiz **ilk kez giriş yapamaz**;
  giriş sonrası saklanan kanonik taslak kayıtları sonraki
  oturumda görüntülenebilir. Nihai offline-first uygulaması için
  cihaz-içi yetkilendirmeyi ayrıca tasarlamak gerekir.
- Sunucudan alınan meralar ana Harita/Alanlar editörüne
  otomatik aktarılmaz; mevcut taslakların üzerine yazılmaz.
  `APPLIED`/Tracker komut kabulü üretilmez.
- Sahip yardımcısı, hayvan ve tasma ataması, geçmiş
  konum/olaylar, cihaz kayıt defteri ve tam iki yönlü
  offline outbox ayrı iş paketleridir.

Bu dosyadaki S6D testlerinin PASS durumu ancak gerçek
kullanıcının PC çıktısından sonra işaretlenebilir.
