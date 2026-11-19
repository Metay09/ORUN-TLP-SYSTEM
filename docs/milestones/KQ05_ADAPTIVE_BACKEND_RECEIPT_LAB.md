# KQ05 — Uyarlanabilir Backend 0x09 makbuz gruplandırma (19 Kasım 2026)

**PR #355 deneysel dal; DRAFT / NO UPLOAD / NO MERGE.**

Bu adım *normal PERIODIC geçmiş* için Backend'in mevcut doğrulanmış
`VerifiedLocalLedger` kayıtlarını mevcut gerçek `BackendReceiptOutbox.enqueue()`
yoluna otomatik, seçmeli gruplar halinde verir. Yeni kriptografi, yeni
on-air mesaj veya Gateway imza yetkisi eklemez. Ürün servisleri/DB değişmez.

- Kaynak: sadece Backend tarafından AES-CCM ile doğrulanıp SQLite'a
  yazılmış `verified_observations` içinden `family=1` ve aynı cihaz,
  incarnation, epoch kayıtları. EVENT kapsam dışı.
- Önceden imzalanıp kalıcı kuyrukta bekleyen sequence kimlikleri yeniden
  sıraya konmaz; `outbox.oldest(1)` bütün outbox'ı yeniden kriptografik
  doğrular, bozuk suffix dahil herhangi bir bozuklukta fail-closed.
- **1 dakika** gibi sık profilde yeni 5 doğrulanmış kimlik varsa 5'li
  Backend receipt. 1..4 kimlik 120 saniye ilk bekleme zamanından sonra
  tekli/2..4 grup olarak kuyruğa alınır. Eksik 103, diğer 101/102/104/105
  için makbuzu bloklamaz; kesinlikle 1..105 aralığına onay verilmez.
- **60 dakika ve üzeri** yalnız *güvenilir Backend rapor profili* ile
  tek kayıt geldiği gibi 1'li makbuz. Radyodaki/tasmanın beyanındaki
  periyot doğrulama kaynağı değildir.
- `poll(now_ms)` çağrı tabanlı ve 32-bit monotonic rollover-safe'dir;
  arka planda zamanlayıcı çalıştırmaz. Sadece yeni makbuzun
  **Backend SQLite outbox**'ına commit edildiği söylenir.
- Birikmiş receipt varsa Edge/Gateway/Tracker'a ulaştığı veya yerel
  Flash'ın serbest bırakıldığı anlamına gelmez. Backend'de 0x09 ile
  Gateway'in ayrı 0x0A CUSTODY ACK'si **birleştirilmez**; farklı otoritelerdir.
- Aynı SQLite writer rezervasyonu ve AEAD + HMAC bağımsız
  `BackendReceiptOutbox.enqueue()` tarafından tekrar uygulanır.
  Kaybolmuş/zayıf doğrulanmış/kısmen bozulmuş kayıtlarda ACK oluşturulmaz.

## PC yalnızca bu dilim

```bash
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_kq05_backend_adaptive_receipt.py
```

**Henüz PC testi bu commit'te operatör tarafından çalıştırılmadı.**
Buradaki pass iddiaları host testi sonrası yapılmalıdır.

## Eksikler / saha engeli

- Deneysel Backend scheduler gerçek güvenilir rapor profili / servis döngüsüne
  bağlanmadı. Kod host-only adapter'dır.
- Backend HTTP→Edge yetkilendirilmiş fetch / gerçek BLE Gateway kurulum
  ve fiziksel SX126x 0x09 downlink/Tracker açık RX penceresi hazır değil.
- Bölgesel toplam RF airtime, düşük güç/uyku, restart/power-cut,
  backend/gateway güvenilir yetki yaşam döngüsü ve 24 saat Flash kapasitesi
  ürün kabulünden geçmedi.
- **LIVE ACK beklemez**, HISTORY'nin bekleme veya kuyruk doluluğu yeni
  konumu kapatmamalıdır. Test edilen 5/5 bypass regresyonu korunur.

**Hedef:** yeni LIVE hemen, eski kayıtlar yalnız ilgili doğrulanmış
kimlikler için seçmeli makbuz, kritik EVENT ayrı güvenli onay.
