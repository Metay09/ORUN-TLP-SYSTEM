# ORUN S4A — PC Edge için ortak veri salt-okunur SQLite aynası

**Durum:** DRAFT — kaynak kodu ve host test koşucusu eklendi;
**kullanıcı Debian host'ta 2026-10-29 tarihinde doğruladı: 8/8 unittest PASS + S3C gerçek HTTP çapraz-dil regresyonu PASS (`7a01360`).**
**Stack:** PR #194 Backend S2 + #195 Step3C kimlik doğrulama + #197 Android S3B
+ #198 gerçek HTTP E2E.
**Çalışan sistem:** `orun-pc-edge.service`, `orun-backend.service`,
mevcut PC Edge defteri ve eski telefon mera kayıtlarına **dokunulmadı**.

## Bu dilim neyi çözüyor?

Ortak alan, grup ve hedef taslağı kayıtları sunucuya yazılıp Android örnek
istemcisine indirilebilse de, bilgisayar/PC Edge için bağımsız ve kalıcı
bir yerel kopya eksikti.

- `tools/orun_shared_pc_mirror.py`: ayrı, özel izinli SQLite veritabanı.
- Sadece `GET /api/v1/sync/identity` (sunucunun doğruladığı kurulum+saha
  bilgisi) ve `GET /api/v1/sync` çağırır. HTTP POST/komut yazma YOK.
- Backend `shared_sync_http.py`'ye yalnız doğrulanmış token'ın kimliğini
  döndüren, query kabul etmeyen `/api/v1/sync/identity` eklendi. Telefonun
  yolladığı sahayı doğrudan yetki kabul etmez; Step3C kayıt defterinden gelir.
- Yanlış token/site/kurulum eşleşirse **hiçbir özel veri çekilmez** ve cursor
  ilerlemez. Bu, kopya farklı bir sahibin token'ı ile yanlışlıkla yeniden
  kullanılmasın diyedir.
- Gerçek TLS doğrulamalı HTTPS dışında yalnız **127.0.0.1** HTTP kabul edilir.
  Uzak Tailscale HTTP istisnası yoktur. Redirect kapalı; token URL'ye konmaz,
  yalnız `ORUN_SHARED_SYNC_TOKEN` ortam değişkeninden alınır. Logda yok.
- Her sayfa: sıralı sequence, kesin cursor, revizyon, silme tombstone'u ve
  Backend S2 E7/kimlik/geometri şeması **önceden** doğrulanır; kaynaklar ve
  cursor **tek SQLite transaction** ile kalıcı hale gelir.
- Eski yerel kaynakları ezmez; aynı revizyonda farklı veri veya bozuk DB
  varsa kapalı kalır. Saklama dosyası `0600`, dosya eşzamanlı kullanım için
  `flock` kilidi; `quick_check` ve mevcut veri doğrulaması bulunur.
- `--once`: **yalnız bir açıkça başlatılmış** eşitleme turu, en fazla 10 sayfa.
  `--list`: yerel, çevrimdışı ve salt-okunur JSON görünümü.
- Backend'deki tasma komutları, `GeofenceMutationOwner`, BLE, USB, LoRa,
  alarm, `APPLIED` veya PC Edge'in var olan observation/upload defteri
  değiştirilmez.

## Test kanıtı bekleyen komut

```bash
bash tools/ci/run_shared_pc_mirror_host.sh
```

`backend/tests/test_shared_pc_mirror.py` gerçek `127.0.0.1` HTTP,
geçici Step3C onay/Ed25519, Backend SQLite ve PC SQLite aynasıyla:
- 3 ortak kaynağı gerçek HTTP'den indirip tekrar açınca koruma;
- revizyon güncellemesi ve incremental resume;
- yanlış saha/başka kurulum token'ını **veri çekmeden** reddetme;
- iptal edilen token 401, yerel kopyayı silmeme;
- salt zona yetkili istemcinin diğer türleri görmemesi;
- bozuk sayfa/çakışma halinde **tam transaction rollback**;
- yerel kimlik kilidi, bozuk JSON'da fail-closed;
- HTTP redirect, uzak cleartext adres ve geçersiz bench token reddi.

S3C Node/TS gerçek HTTP E2E de regression olarak tekrar çalışır.

## Kullanıcının gerçek test çıktısı (2026-10-29)

- `git fetch origin feat/pc-edge-shared-read-mirror` → detached `7a01360`.
- `bash tools/ci/run_shared_pc_mirror_host.sh` → **Ran 8 tests in 5.865s — OK**.
- `SHARED_SYNC_REAL_HTTP_E2E: PASS`: owner 3/3 upload, read-only B 3/3 pull, site isolation, revision conflict, viewer reject, restart.
- `STEP3C_REAL_HTTP_E2E: PASS`: revoked credential HTTP 401, other credential remains authorized.
- Final line `ORUN_S4A_HOST_PASS`. No real user site token, running Backend/PC Edge instance, real Android or Tracker changed in this run.
- Note: user's interactive Bash startup `~/.bashrc` independently had a dangling quote near line 117; new test still ran and passed. Do not alter shell config through product test scripts.

## Sonrasında

1. İzole PC testinin host PASS kanıtı; gerçek kullanıcı verisine yükleme YOK.
2. Sahibin onayıyla Backend'de gerçek Step3C kayıt/yönetim arayüzü,
   token rotasyonu, TLS sertifika/hostname, site/Device Registry izinleri.
3. Aynı PC Edge servisinin yaşam döngüsüne bağımsız shared-mirror iş
   döngüsünü entegre etme; restart/offline recovery ve işletim logları.
4. Android SQLite'de gerçekten atomik outbox, gerçek enrollment ve
   iki fiziksel telefonla senkronizasyon. Ardından yalnız doğrulanmış
   Tracker RESULT'uyla geofence uygulama.

**NOT:** S4A salt-okunur PC yardımcı aracı ürünün son hali değildir.
Geçici yerel test anahtarlarını asla canlı sunucuda kullanmayın.
