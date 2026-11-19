# KQ05 — 19.11.2026 ACK kaybı / kritik olay hızlı onay PC adımı

**DRAFT / opt-in / DONANIM YOK / PASS HENÜZ DOĞRULANMADI.**
Temel: PR #353, commit `62156fee1fee88c9b7899c2f46e3f83d01201a90`.
`docs/architecture/ADR_KQ05_LIVE_HISTORY_CONTROL.md` kararları korunur.

| Kayıt | Gönderim | Onay | Kalıcı silme yetkisi |
| --- | --- | --- | --- |
| Yeni PERIODIC LIVE | Şifreli, ACKless, yeni konum önce | Per-frame Gateway ACK yok | Yalnız doğrulanmış Backend 0x09 açık kimlikli makbuzu |
| Eski PERIODIC HISTORY | LIVE boşluğunda, sınırlı yeniden TX; custody flag **kapalı** | Backend 0x09, 1..8 seçmeli kimlik; **henüz uçtan uca dağıtımı bağlı değil** | Tracker SecurityStore doğrulaması ve tek tek OBS1 kalıcı marker |
| Kritik EVENT | Custody requested, tekrar edilebilir exact byte | **Deneysel 0x0A count=1**: OCQ2 durable readback sonrası ilk fırsatta; varsayılan firmware eski 0x05'i kullanır | HMAC / kapsam / exact object doğrulaması ve OBS1 marker |
| Ayarlar / alan / güvenlik | Ayrı imzalı kontrol protokolü | Uygulanmış komut sonucu | Konum makbuzuyla ilişkisi yok |

## Bu dalın uyguladığı sınırlı düzeltmeler

- Deneysel Gateway 0x0A runtime, bir EVENT'i OCQ2 üzerinde **kalıcı** gördüğü anda `requestFlush()` çağırır. EVENT birikim için beşinci kaydı veya iki dakikalık kısmi timeout'u beklemez. RF downlink'i başlatılmış olması tasmanın ACK aldığı anlamına gelmez.
- Düşen EVENT downlink'inde aynı exact EVENT tekrar gönderildiğinde Gateway OCQ2 kayıt tekrarı yazmadan hemen yeni 1-kayıt onayı hazırlayabilir.
- PERIODIC custody için 1..5 grup/timer host davranışı aynen korunur. Normal PERIODIC LIVE/HISTORY custody **istemez**; bu olay-grubu mekanizması **normal geçmişin toplu makbuzu değildir**.
- Normal geçmişte 0x09 Backend makbuz yolu kalır, fakat Backend -> Edge -> Gateway -> Tracker gerçek haberleşme zinciri tamamlanmadan "uçtan uca güvenilir geçmiş" ilan edilmez.
- 30 dakika / bir dakika raporlama regresyonu: beş eski kayıt, 4/4 exact cache dolu, hiç ACK yok; 6..35 yeni LIVE gönderimi ilerler. HISTORY alt öncelikte, hiçbir release marker sahte üretilmez.

## Kullanıcı PC doğrulama kapısı

```bash
bash tools/ci/run_kq05_gateway_batch_runtime_host.sh
bash tools/ci/run_sf5k3v_tracker_tx_host.sh
bash tools/ci/run_kq05_tracker_batch_radio_runtime_host.sh
bash tools/ci/run_kq05_lab_preflight.sh
```

Bunlar firmware ana dalına otomatik geçiş, gerçek RAK upload, Flash silme veya #221 P0 saha kabulü değildir. Kalan blokörler: gerçek iki uçlu RF/uyku penceresi, Backend 0x09 downlink zinciri, RF bütçesi, birden fazla Tracker fairness, gerçek NOR/enerji ölçümü.
