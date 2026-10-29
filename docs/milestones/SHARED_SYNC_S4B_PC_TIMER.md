# ORUN S4B — PC Edge ortak veri okuma zamanlayıcısı, güvenli hazırlık

**Durum: DRAFT / HOST TESTS NOT RUN.** Önceki S4A `7a01360` testleri
kullanıcının bilgisayarında 8/8 PASS; S3C gerçek HTTP E2E PASS.
Bu dalda eklenen zamanlayıcı ve özel token dosyası **henüz doğrulanmadı**.

## Tamamlanan kod

- PC ortak veri aynası için **özel token dosyasından** 0600, kullanıcı sahipliğinde
  bearer okuma desteği `tools/orun_shared_pc_mirror.py --token-file PATH` ile
  eklendi. Token ne CLI argümanında ne unit dosyasında ne de çıktıdadır.
  Belirtilirse yanlış ortak bench ortam değişkeni göz ardı edilir.
- `tools/ci/render_shared_pc_timer.py` yalnızca **belirtilen ayrı staging
  klasöründe**, var olan dosyaların üzerine yazmadan iki yeni aday dosya
  oluşturur: `orun-shared-pc-mirror.service` ve
  `orun-shared-pc-mirror.timer`.
- Zamanlama: ilk açılıştan sonra 60s; son çalışmanın bitiminden sonra 180s,
  30s rastgele gecikme. Bir tur `--once` en fazla 10 sayfa indirir.
- Mevcut `orun-pc-edge.service` **aynen kalır**, `orun-backend.service`
  değişmez; yeni unitler kurulmaz, aktive edilmez, `systemctl` çağrılmaz,
  USB/BLE/LoRa ve Tracker'a hiçbir komut gitmez.
- Sadece E7 alan, grup, hedef taslaklarını okur. Yetki Backend'deki
  Step3C kurulum kaydına bağlı. Plain HTTP yalnız 127.0.0.1 host testinde;
  uzak sunucu için geçerli HTTPS sertifikası gerekli.

## Test

```bash
bash tools/ci/run_shared_pc_timer_host.sh
```

- S4A gerçek HTTP/SQLite temel regressions **9 test** (ek bir token-file
  real-HTTP CLI testi dahil).
- S4B ayrı hazırlık/özel bearer testleri **3 test**.
- S3C Node/TypeScript ↔ Python gerçek HTTP E2E tekrar çalışır.
- Toplam **12 unittest** bekleniyor. Sonuç kullanıcı host'una bağlı; PASS
  önceden iddia edilmiyor.

## Taslakları üretme (daha sonra, gerçek yetkili PC kaydı hazır olunca)

Aday unitlerin üretilmesi tek başına bir servis başlatmaz. Gerçek sahaya
kurulum için per-installation token, owner-approved enrollment ve HTTPS URL
gerekli; mevcut test tokenları üretimde kullanılamaz. Ayrı ve yeni bir
**yalnızca ortak veri** SQLite yoluna ihtiyaç vardır.

```text
python3 tools/ci/render_shared_pc_timer.py \
  --repo /path/to/ORUN-TLP-SYSTEM \
  --output-dir /private/staging-dir \
  --db /private/state-dir/shared-pc-mirror.sqlite \
  --site APPROVED_SITE \
  --installation APPROVED_PC_EDGE_ID \
  --url https://valid-authorized-backend.example \
  --token-file /private/secrets/pc-edge-enrolled-token
```

Sadece staging dosyası üretir; kullanıcı onayı olmadan
`~/.config/systemd/user` veya `/etc/systemd` içine hiçbir şey kopyalanmaz.
`systemctl --user enable`/restart yapılmaz. Önce yetki, servis
ortamı, sertifika ve yeni veritabanı izinlerini ayrıca doğrulamak gerekir.

## Açık ürün işleri

- Üretim Step3C enrollment/revocation ve HTTPS/Tailscale sunucu dağıtımı.
- Gerçek yetkili telefon ve PC eşitlemesinin arayüze bağlanması.
- Ortak veri aynasının PC'deki **harita/alanlar ekranına** okunur kaynaktan
  gösterilmesi; bu dal henüz görüntüleme UI'sını değiştirmez.
- Android SQLite güvenli outbox ve USB/BLE/LoRa Tracker uygulama RESULT
  hattı. Bu dalda `APPLIED` iddiası veya geofence komutu yok.
