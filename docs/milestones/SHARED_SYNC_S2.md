# ORUN S2 — Backend ortak alan/grup/hedef-taslağı senkronizasyon çekirdeği

**Durum:** IMPLEMENTATION CANDIDATE; **owner Debian host tests PASS (19/19, 2026-10-29, commit `e3e3b89`)**. Actual enrolled phones/production backend still NOT integrated.
**Bağımlılık:** `docs/architecture/ORUN_SHARED_DATA_SYNC_AND_GEOFENCE_DELIVERY.md`.
**Ön koşul:** Gerçek istemci oturumlarının Step 3C ile ayrı ayrı onaylanması;
bu PR kimlik doğrulama sistemi kurmaz.

## Kod

- `backend/shared_sync_store.py`: SQLite-backed domain owner.
- `backend/shared_sync_http.py`: principal resolver **zorunlu** HTTP adapter.
- `backend/tests/test_shared_sync.py`: çift istemci, yeniden açma,
  çakışma, silme/tombstone, idempotent retry, ACL/saha izolasyonu,
  taslak hedefler, geometri, HTTP testleri.
- `tools/ci/run_shared_sync_host.sh`: root seviyesinden host test başlatıcı.

## Kesin sınır

Mevcut `backend/orun_backend_bench.py`, `orun-backend.service`,
`orun-pc-edge.service`, mevcut sunucu SQLite tablosu,
`edge/app/src/core/` ve firmware **değiştirilmez**.

Bu kod yeni, ayrı SQLite yoluna işaret edilmeden hiçbir veriyi değiştirmez.
`SharedStore`'un kendi başına CLI sunucusu yoktur. HTTP adapter yalnız
`handler_factory(store, resolve_actor)` ile gerçek, dışarıdan doğrulanmış
`Actor(site_id, actor_id, installation_id, scopes)` geldiğinde kullanılabilir.
**Header içinden site_id/actor_id almak veya bench shared bearer token'ını
çok kullanıcı yetkisi saymak yasaktır.** Eksik principal durumunda 401.

Kısmi kaynak türleri:
- `zone` (ad, active, E7 koordinatlı sağlam çokgen)
- `group` (ad, cihaz id listesi; grup üyeliği şimdilik seçme taslağı)
- `zone_target` (hangi alan için scope=all/selected, cihaz/grup id listesi;
  **yalnız hedef taslağı**; tasma uygulanmış/teslim edilmiş sayılmaz)

API S2 (Step 3C auth adapte edildiğinde):
- `POST /api/v1/sync/ops`, body `{"ops":[...]}`.
- `GET /api/v1/sync?cursor=0&limit=100`.
- İşlemler: `{"op_id":"...", "kind":"zone", "id":"...", "action":"UPSERT",
  "base_revision":0, "body":{"name":"...", "active":true, "ring_e7":[[latE7,lonE7],...]}}`.
- Silme: action=DELETE, body=null, eski tam revision şart.
- Sonuç: ACCEPTED + revision + sequence / CONFLICT + revision /
  REJECTED; `op_id` aktör/kurulum/saha bağlamında kalıcı tekrarsız.
- Pull yalnız principal'ın yetkili olduğu kaynak türleri ve sahadan
  sıralı değişimleri döndürür; tombstones korunur.
- `zone_target` verisindeki cihaz ID'sini kaydetmek cihaz üzerinde
  komut yetkisi vermez. Gerçek device registry sahipliği ve imzalı
  COMMAND/RESULT ayrı güvenlik aşamasında zorunludur.
- Kapsamlı fiziki Android offline outbox, schema migration ve per-install
  enrollment bu dilimde **yok**.

## Owner host validation evidence (2026-10-29)

- `bash tools/ci/run_shared_sync_host.sh`: **19 tests, 0 failure** (5.514s).
- Git SHA under test: `e3e3b89`.
- Test scope: site isolation, read/write ACL, two-client HTTP pull, revision conflicts, tombstone replay protection, idempotent retry, invalid E7 geometry, zone-target draft, pagination, reopen SQLite.
- This is NOT multi-phone live synchronization; device enrollment, authenticated routing, Android offline outbox, real production server and Tracker mutation remain unimplemented.

## Kontrol

Repo root'unda:
```bash
bash tools/ci/run_shared_sync_host.sh
```

Gerçek kabul daha sonra: Telefon A'nın offline kaydetmesi, Backend'e aktarım,
Telefon B ve web'in yetkili revizyonu çekmesi; farklı sahaların 403/boş yetkili
sonuç alması; çift offline geometri revizyonunda 409/CONFLICT; cihaz
`APPLIED` demeden uygulama rozeti olmaması.

**Henüz üretim sunucusuna açılmayacak, Tailscale'ye servis olarak
bağlanmayacak ve mevcut uygulama verileri otomatik taşınmayacak.**
