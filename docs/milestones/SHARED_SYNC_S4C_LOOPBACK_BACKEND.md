# ORUN S4C — bağımsız ortak veri Backend süreci (127.0.0.1)

**Durum:** HOST TESTLERİ PASS (2026-10-29): kullanıcı Debian `be1e7ef`, **16/16 unittest PASS**, S3C Python↔Node gerçek HTTP E2E PASS. Canlı sisteme kurulum hâlâ YOK.
**Önceki kanıt:** S4B kullanıcının Debian bilgisayarında **12/12 PASS**
(`d5ef4d0`), S3C gerçek Python↔Node HTTP **PASS**.
**Üretim servisi:** Çalışan `orun-backend.service` ve
`orun-pc-edge.service` DEĞİŞMEZ.

## İşlevsel eksiklik kapatılıyor

Önceden `SharedStore` + `EdgeEnrollment` + HTTP adaptörü yalnız host
testlerinin başlattığı geçici sunucuyla kullanılıyordu. `tools/
orun_shared_sync_loopback.py` bunları birbirine bağlayan gerçek, ayrı,
tekrar açılabilir bir **komut satırı sunucu süreci** sağlar.

- Yalnız `127.0.0.1`, isteğe bağlı port (`0` = rastgele). `0.0.0.0`,
  dış Tailscale adresi veya `localhost` ismi reddedilir.
- Sahibin önceden oluşturduğu, sahipliği kendisine ait **0700** özel
  bir dizin gerekir. İçinde sıfırdan **0600** iki ayrı SQLite dosyası:
  `shared_resources.sqlite`, `enrolled_edges.sqlite`. Bunlar çalışan
  ORUN observation/Edge defteri değildir.
- İlk başlatma yalnız dizin boşken yapılır. Sunucu kapatılıp yeniden
  çalıştırılırken `--reuse-state` **açıkça** verilir; eksik çift,
  bağlaşmayan izinler ve symlink reddedilir; eski kayıt silinmez.
- HTTP, Backend `handler_factory(..., enrollment.resolve_headers)` ile
  **yalnız gerçek Step3C kurulum token'ını** kabul eder. Bench token, taklit
  `X-Site` / `X-Actor` başlıkları yetki vermez.
- Sahip onayı ve yeni telefon/PC kaydı **bu HTTP sürecinde açılmaz**.
  Bunlar ayrı, sahip denetimli güvenli kayıt işlemi olmadan ilerlemez.
- `/api/v1/sync`, `/api/v1/sync/ops`,
  `/api/v1/sync/identity` bütün önceki işlem/revizyon/saha haklarıyla
  kullanılabilir. Backend ACCEPTED tasma APPLIED demek değildir.
- Düz HTTP yalnız loopback içindir. HTTPS reverse proxy ve Tailscale
  erişimi **henüz yoktur**; bu alt yapı doğrudan saha servisi değildir.

## Güvenli host kabul testi

```bash
bash tools/ci/run_shared_sync_backend_runtime_host.sh
```

Test şunları oluşturup yok eder: özel geçici dizin, gerçek Python
sunucu alt süreci, 2 SQLite Backend dosyası, yetkili telefon/PC Ed25519
kurulumları, ayrı PC local mirror. Dış porta bağlanmaz.

Test senaryoları: aktif tokenla HTTP/PC aynası, yetkisiz 401 ve sahte
başlık reddi, farklı site izolasyonu, kayıtların Python sunucu yeniden
başlatılmasından sonra kalıcılığı, iptal edilen PC'nin 401 alması ve
diğer kurulumların etkilenmemesi, genel enrollment endpoint'i olmadığının
kanıtı, public bind/yanlış izin/symlink/yarım SQLite çiftini reddetme.
S4B 12 regresyonu ve S3C TS↔Python gerçek HTTP de tekrar çalışır.

**Beklenen:** 4 yeni test + 12 önceki = 16 unittest; ardından S3C e2e.
Bu PR için **PASS iddiası henüz yok**.

## Kullanıcı host doğrulaması (2026-10-29)

- `git fetch origin feat/backend-shared-sync-loopback-runtime` → detached `be1e7ef`.
- `bash tools/ci/run_shared_sync_backend_runtime_host.sh` → **Ran 16 tests in 7.342s — OK**, son satır `ORUN_S4C_HOST_PASS`.
- S3C Node TypeScript ↔ Python HTTP E2E **PASS**: owner 3/3 push, viewer 3/3 pull, cross-site isolation, conflicts retained, viewer write rejected, restart persists.
- Step3C revocation: 401 revoked, separately enrolled phone remains authorized.
- `127.0.0.1` ve geçici SQLite kullanıldı; ORUN canlı Backend ve PC Edge systemd servisleri ve Tracker değişmedi.

## Gerçek ürüne geçişi engelleyen sınırlar

- Sahip onaylı kayıt UI/CLI ve özel per-installation token dağıtım kanalı.
- TLS sertifika, hostname/pinning değerlendirmesi ve giriş kontrolü.
- Canlı observation Backend sistemine servis rotası entegrasyonu.
- Gerçek Android otomatik senkronizasyonu ve alan işlemlerinin atomik
  SQLite outbox entegrasyonu.
- Edge/firmware'e onaylı geofence komutu ve cihazdan gerçek RESULT/APPLIED
  kanıtı; fiziksel GNSS/LoRa saha testleri.

**S4C bir üretim dağıtımı değildir.** User-facing ürün bitmiş sayılmaz.
