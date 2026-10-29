# ORUN S4D — yetkili yerel PC Edge kurulum kimliği, gerçek ortak veri

**Durum: DRAFT / HOST PASS (2026-10-29, Debian): 20/20 unittest PASS, S3C gerçek HTTP/Node regresyonu PASS. Canlı servise dağıtım YOK.**
**Önceki kanıt:** S4C `be1e7ef` **16/16 PASS** + gerçek
Python ↔ Node TypeScript HTTP E2E **PASS** (2026-10-29).
**Canlı sistem:** `orun-backend.service`, `orun-pc-edge.service`,
gerçek Gateway/Tracker, Android APK ve sahip kullanıcı verisi **değişmez**.

## Pratik ürün engeli

S4C iki ayrı SQLite'ye sahip kalıcı HTTP Backend'i ve S4A/B yetkili
PC kaynak aynasını sağladı. İkisinin gerçekten **ayrı PC kurulum yetkisi**
ile birleştirilmesi için server operatörünün yerel, açık onaylı kimlik
üretimi gerekiyordu. Bu dal tam olarak bu küçük boşluğu kapatır.

`tools/orun_shared_local_pc_enroll.py` yalnız Backend PC'sinin **güvenilir
yerel operator oturumunda**, `--authorize-local-pc` açıkça verilirse
çalışır. Böylece anonim HTTP üzerinde owner approval kesinlikle açılmaz.

- Sadece **`pc-*`** installation ID'leri. Telefon, Gateway veya Tracker
  kayıt yolunu etkilemez; kurulum kapsamı **yalnız** `zone:read`,
  `group:read`, `zone_target:read`. Yazma ya da Tracker komutu yok.
- Önceden var olan sahibi tarafından korunmuş **0700** S4C state dizini
  ve **0600** Backend SQLite çifti gerekir. Hiçbir yeni Backend veritabanı
  açılmaz; canlı gözlem defterine gidilmez.
- `Ed25519PrivateKey.generate()` ile oturum boyunca tutulan geçici
  anahtar, gerçek `EdgeEnrollment.begin → approve → finish` ve
  `signed_challenge` kanıtı üretir. Bu işlem yalnız owner'ın açık
  local-shell komutuyla yapılabilir. Özel Ed25519 anahtarı ve owner onay
  kodu diske yazılmaz.
- Sunucunun ürettiği birim başına **30 günlük** token **yeni,
  yalnız sahibin erişebildiği 0700 klasördeki** bir **0600** dosyaya
  atomik olarak değil, doğrudan ve fsync ile yazılır. Var olan token
  dosyasının üzerine yazılmaz; symlink, yanlış izin, yanlış kullanıcı
  reddedilir. Token komut argümanında, stdout/stderr veya systemd unit'te
  görünmez.
- Token yazımı başarısızsa mümkün olduğunca aynı kurulum hemen iptal
  edilir. İptal de başarısız olursa manuel güvenlik incelemesi gerekir.
- Bu pilot yöntem özel anahtarı saklamadığı için başka cihazda tekrar
  imzalama/kayıt yapma yöntemi değildir. Yetki yenileme/cihaz değiştirme
  için ayrıca üretim politikası gerekir. **Android Keystore ve gerçek
  uzaktaki PC kaydı yerine geçmez.**

## Güvenli host testi

```bash
bash tools/ci/run_shared_local_pc_enrollment_host.sh
```

Geçici SQLite + gerçek 127.0.0.1 HTTP sunucu ile:

1. Açık `--authorize-local-pc` verilmeden **hiçbir** kayıt/token oluşmaz.
2. Ayrı Python CLI `pc-01` kurar, 0600 dosyada saklar, gerçek Backend'den
   kendi site+kurulum kimliğini görür; yazma operasyonu **REJECTED**.
3. PC Edge aynası gerçek HTTP'den merayı alıp tekrar aynı cursor'dan
   no-op çalışır; token stdout/stderr'e sızmaz.
4. `pc-01` iptal edilince 401; bağımsız `pc-02` yetkili kalır.
5. Telefon/çapraz yol, symlink, var olan dosyanın üzerine yazma ve
   group-readable token parent klasörü reddedilir.
6. S4C/S4B/S4A için **16 unittest** ve S3C Node TypeScript ↔ Python
   gerçek HTTP regresyonu tekrar çalışır.

Beklenen **4 yeni + 16 önceki = 20 unittest**, sonrasında S3C regresyonu;
bu sürüm için aşağıdaki kullanıcı PASS kanıtı alınmıştır.

## Kullanıcı doğrulaması — 29 Ekim 2026

- Kullanıcı `feat/shared-sync-local-pc-enrollment` başını `a750427` olarak fetch etti.
- `bash tools/ci/run_shared_local_pc_enrollment_host.sh`: **20 test / 20 PASS (11.788s)** ve `ORUN_S4D_HOST_PASS`.
- Gerçek `127.0.0.1` Python↔Node HTTP: owner 3/3 upload, viewer 3/3 pull, cross-site isolation, viewer write rejected, conflict/restart preserved, revoked token 401; diğer token yetkili.
- İzole `TemporaryDirectory` + SQLite ve test tokenı kullanıldı; mevcut `orun-backend.service`/`orun-pc-edge.service`, kullanıcı telefonundaki veriler, Gateway/Tracker değişmedi.
- Telefon ekranları ayrıca değerlendirilmiştir: eski görüntüde 3.802 yerel kayıt; sonraki 20:25 Ağ/Harita görüntülerinde **0 kayıt**, **sunucu ayarlanmamış**, canlı Backend bağlantısı doğrulanmamış. Bu ekranlar S4D host testinin gerçek telefonda çalıştığı anlamına gelmez.

## Üretim öncesi eksikler

- Gerçek PC Edge'in farklı bilgisayarda olabileceği mimari için HTTPS +
  üretim owner approval/remote enrollment; buradaki **lokal trusted operator
  bootstrap farklı PC'yi otomatik kaydetmez**.
- Canonical Backend'i mevcut observation backend'e güvenli şekilde dahil
  etme veya ayrı servis/HTTPS proxy kurma.
- Timer (S4B) adayını sahibin onayıyla ayrıca aktifleştirme; canlı PC
  Edge servisinden bağımsız kalması.
- Android outbox/UI/Keystore/telefon kaydını gerçek sunucuya bağlama.
- Sonradan USB/BLE/LoRa command dispatch ve güvenilir Tracker RESULT.

**Kod, Merge ve dağıtım farklı şeylerdir.** Bu PR taslak kalacak;
işlevlerin birinci gerçek sahaya alınması sahibin izin ve kapsam kararını
gerektirir.
