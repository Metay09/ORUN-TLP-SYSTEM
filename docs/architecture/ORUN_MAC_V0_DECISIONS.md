# ORUN MAC v0 — Gönderim, ACK ve Relay Zamanlama Kararları

**Durum:** ÖNERİ. Sahip (Metay) onaylarsa DONDURULUR.
**Tarih:** 2026-10-16
**Kapsam:** Tracker → (isteğe bağlı tek relay) → Gateway yolunda veri gönderimi, custody ACK dönüşü, backlog boşaltma ve çakışma kuralları.
**Değişmeyenler:** TLP v1 baytları, SF5B PRODUCT_SECURE / V2_RELAY_FORWARD / GATEWAY_CUSTODY_ACK wire formatları, SF5K0 §8.1 "relay sadece taşıyıcıdır" kararı. Bu doküman yeni bir paket alanı eklemez.
**Kanıt:** `tools/mac_sim/orun_mac_sim.py` (yalnız bilgisayarda çalışan zamanlama simülatörü). Fiziksel RF, akım veya mevzuat uyumu kanıtı DEĞİLDİR.

Bu doküman SF5K0 §9–10'da açık bırakılan soruları **karara bağlamak** için yazıldı. Oradaki tespitler doğrudur; eksik olan bir karar setiydi.

---

## 0. Tek cümle

> **Tracker konuşur, cevap sabit bir zamanda gelir. Kaçan cevap veri kaybı değildir, sonraki denemedir.**

---

## 1. Kurallar

### R1 — Sabit ACK randevusu (slot)

- Tracker gönderir, radyosu uyur ve **TX_bitişi + X** anında **w** genişliğinde kısa bir pencere açar. ACK'in preamble'ı bu pencerede başlamazsa pencere kapanır.
- Bugünkü "TX'ten sonra 10 s sürekli dinle" (`kWindowedRxAfterTxMs`) davranışının yerini alır.
- **X** cihaz genelinde tek bir ayardır ve SF'ye bağlıdır:

  `X ≥ relay_gecikme_max + sarılı_uplink_airtime + gateway_commit_max + ACK_airtime + 1 s pay`

  | SF | PERIODIC | sarılı | ACK | sarılı ACK | X_min | **X (v0)** |
  |---|---:|---:|---:|---:|---:|---:|
  | 8  | 0,308 s | 0,349 s | 0,195 s | 0,236 s | 6,24 s | **7 s** |
  | 9  | 0,554 s | 0,615 s | 0,349 s | 0,431 s | 6,66 s | **7 s** |
  | 10 | 1,026 s | 1,149 s | 0,657 s | 0,780 s | 7,51 s | **8 s** |
  | 11 | 2,216 s | 2,462 s | 1,397 s | 1,642 s | 9,56 s | **10 s** |

  Varsayımlar: relay gecikmesi 1,2–4,2 s (bugünkü `relay_config.h`), gateway commit en fazla 0,5 s. Airtime değerleri SF5B §18 tablosuyla bire bir aynıdır (simülatör `--selftest`).
- **w = 0,5 s** başlangıç değeridir ve donanımda ölçülecektir.

**ACK'i kim zamanlar?**
- **Direkt yol:** Gateway, tracker paketini bitirdiği an + X'te ACK'i gönderir.
- **Relay yolu:** Gateway ACK'i hazır olur olmaz relay'e gönderir. Relay ACK'i **tutar** ve kendi duyduğu tracker paketinin bitişi + X anında gönderir. Gateway'in tracker'ın zamanını bilmesi gerekmez.
- **Aşağı yönde rastgele relay gecikmesi YOKTUR.** SF5K0 §9.4'teki "10 s yetmez" sonucu, downlink'e de 1,2–4,2 s rastgele gecikme koyduğu için çıkıyordu. v0'da relay ACK'i tam slot anında attığı için SF11'de bile X = 10 s yetiyor. Ağır yükte (30 tasma, SF11) birkaç "relay geç kaldı" görüldü; X sahada ölçülerek ayarlanır.
- **Slot önceliği:** Relay ve gateway, rezerve edilmiş bir ACK slotuyla çakışacak hiçbir forward veya ACK gönderimini başlatmaz. Gönderim slottan sonraya kayar.

### R2 — Kaçan ACK = tekrar deneme, kayıp değil

- Kayıt, geçerli bir custody ACK gelene kadar silinmez. Bu zaten mevcut sözleşmedir.
- Tekrar deneme **aynı ciphertext** ile yapılır (SF5B §10). Gateway `(device, seq)` ile dedupe eder. Kayıt zaten durable ise ACK'i beklemeden yeniden üretir.
- Geç gelen ACK geçersiz değildir, sadece duyulmamıştır. Bedeli bir paket airtime'ıdır.

### R3 — Ne zaman, ne kadar, hangi sırayla

- **Ne zaman:** Rapor zamanı = uyanma. Backlog ayrı bir uyanma yaratmaz.
- **Sıra:** Önce bu uyanmanın canlı kaydı, sonra en eski backlog.
- **Ne kadar:** Uyanma başına en fazla **N_MAX = 4** kayıt (v0).
- **Gönderim şekli:** Her kayıt kendi ACK slotunu bekler (stop-and-wait). Sonraki kayıt ancak ACK geldikten sonra gönderilir.
- **ACK kaçarsa:** 2–8 s rastgele bekle, **1 kez** tekrar dene. Yine kaçarsa uyanma biter, kalanlar sonraki rapor zamanına kalır.
- **Gateway kapalıyken:** Uyanma başına en fazla 2 boşa deneme yapılır. Bu, kendiliğinden sınırlı bir yoklama (bounded probing) olur ve sürekli tekrar döngüsü oluşmaz.
- **Neden burst yok:** Relay 1. paketi iletirken tracker 2. paketi atmaz, çünkü 2. paket ancak 1.'nin ACK'inden sonra gider. SF5K0 §9.3'teki çakışma yapısal olarak ortadan kalkar.
- **Batch ACK (SF5K) iptal edilmedi.** v1 optimizasyonudur. Simülatör ve saha ölçümü stop-and-wait'in yetmediğini gösterirse açılır.
- Simülatöre göre SF9'da N_MAX = 8 backlog'u daha hızlı boşaltıyor. 30 tasma, 6 saatlik kesinti, CAD açık, 3 tohum: N_MAX = 4 ile yaklaşık 4,1 saat, N_MAX = 8 ile yaklaşık 3,3 saat. Relay ve gateway duty %6,5'in altında kalıyor. Kesin değer, SF sahada belirlendikten sonra seçilecek.

### R4 — Yol işareti: mevcut `RELAY_ALLOWED` biti

SF5B §5'teki authenticated `path_flags.RELAY_ALLOWED` biti yol işareti olarak kullanılır. **Wire değişikliği yoktur.**

| Durum | Sonraki gönderimde `RELAY_ALLOWED` |
|---|---|
| Son ACK **direkt** gateway'den geldi | 0 |
| Son ACK **relay** üzerinden geldi | 1 |
| ACK kaçtı | 1 |
| İlk açılış | 0 (önce direkt denenir) |

- Relay, `RELAY_ALLOWED=0` olan paketi iletmez. Gateway tracker'ı direkt duyuyorsa gereksiz forward, gereksiz airtime ve çakışma olmaz.
- Simülatör S3: tasma hem gateway'i hem relay'i duyuyor → relay duty %0.

### R5 — Gateway ve relay aynı paketi duyarsa

- Gateway `(device, seq)` ile dedupe eder ve ilk kopya kazanır.
- **Bir deneme için tek ACK yolu vardır:**
  - Gateway direkt kopyayı aldıysa → direkt slot, ACK `RELAY_ALLOWED=0`.
  - Yalnız relay kopyası geldiyse → relay'e gönderilir, ACK `RELAY_ALLOWED=1`.
- Sonradan gelen relay kopyası yalnız gözlem (observation: RSSI/SNR, yol) olarak kaydedilir ve ikinci bir ACK üretmez.

### R6 — Relay davranışı

- **Sadece taşıyıcı** (SF5K0 §8.1 aynen geçerli): ACK üretmez, değiştirmez, custody vermez.
- **Dedupe süreli olmalıdır (yaklaşık 30 s).** Aynı `(device, seq)` daha sonra tekrar gelirse (retry) yeniden iletilmelidir.
  - ⚠️ Bugünkü `PacketDedupeCache<16>` sayı tabanlıdır ve süresi yoktur. v2 relay'e aynen kopyalanırsa, az tasmalı bir sürüde backlog tekrar denemeleri relay'de sessizce yutulur.
- **ACK tutma:** Tracker başına en fazla 1 bekleyen slot vardır. Slot zamanı geçmişse ACK atılır, tracker zaten tekrar deneyecektir.
- Forward kuyruğu: v0'da 4 (bugünkü değer). Simülasyonda hiçbir senaryoda taşmadı (30 tasma, tek relay dahil).

### R7 — Zamanı dağıtma

- Rapor zamanına **aralığın ±%10'u** kadar rastgele kaydırma uygulanır (15 dk için ±90 s).
- Simülatörde sabit ±30 s ile iki tasma saatlerce aynı anda uyanıp her saat çakıştı. S10'da (1 saat aralık) ±30 s yerine ±%10 kullanınca, en kötü tohumda kesinti sonrası boşalma süresi 352 → 223 dk'ya indi.

### R8 — Göndermeden önce kanalı dinle (CAD)

- Tracker uplink, relay forward ve gateway'den relay'e giden ACK öncesinde CAD yapılır. Kanal meşgulse 0,3–2 s beklenir, en fazla 8 kez.
- **Slot ACK'leri CAD yapmaz**, çünkü zamanında gitmeleri gerekir.
- CAD gizli düğümleri (birbirini duymayan tasmaları) çözmez, sadece çakışmayı azaltır.
- Simülatörde: SF9 ve 30 tasmada ACK başarısı %81,5 → %88,1.

### R9 — SF sabit değildir; sahada ölçülerek seçilir

- SF11 bugünkü varsayılandır, ürün kararı değildir.
- Hedef: sahadaki tüm linkler için güvenilir olan **en düşük SF** ve bir pay.
- v0: tek kanal, tek SF, bütün cihazlar aynı SF'de. Link başına ADR yoktur.
- **Bu, kararların en etkilisidir:**
  - 30 tasma tek relay üzerinden: SF11'de relay duty **%19,4** (sınırın neredeyse 2 katı), ACK başarısı %31, backlog erimiyor.
  - Aynısı SF9 ve CAD ile: relay duty **%4,0**, ACK başarısı %83, p95 gecikme 15 s.

### R10 — Duty ve öncelik

- Her verici (tasma, relay, gateway) kendi TX süresini kayan 1 saatlik pencerede sayar. Sınır: BTK 869,4–869,65 MHz için %10, ya da LBT+AFA ile.
- %8'e gelince düşük öncelikli trafik (backlog) durur. Canlı kayıt ve EVENT devam eder.
- **Öncelik:** EVENT (sınır ihlali vb.) > canlı kayıt > backlog. EVENT de R1 slot kuralını kullanır.

---

## 2. Sorulan soruların cevabı

| Soru | Cevap |
|---|---|
| Biriken paketler **ne zaman** gönderilecek? | Normal rapor uyanmasında, canlı kayıttan sonra (R3) |
| **Ne kadar** gönderilecek? | Uyanma başına en fazla N_MAX; ilk kaçan ACK'te 1 tekrar, sonra dur (R3) |
| Relay üzerinden aktarım nasıl? | Tracker `RELAY_ALLOWED` ile ister, relay iletir; ACK'i relay tutup slotta verir (R1, R4, R6) |
| Relay ve gateway aynı anda duyarsa? | Dedupe; tek ACK yolu, direkt öncelikli; sonraki gönderimde relay devre dışı (R4, R5) |
| ACK ne zaman gelecek? | Her zaman TX_bitişi + X'te; X tek ayar (R1) |
| Pencere kaçarsa? | Veri kaybı yok; aynı ciphertext ile tekrar (R2) |
| Relay 2. paketi alırken 1.'yi gönderirse? | Olmaz: tracker 2. paketi 1.'nin ACK'inden sonra atar (R3). Başka tasmanın paketi gelirse bu olağan bir çakışmadır, CAD azaltır, tekrar deneme kurtarır (R2, R8) |

---

## 3. Simülatör sonuçları

Ayarlar: 24 saat, 15 dk rapor aralığı (en kötü durum, hayvan hep hareketli), N_MAX = 4, 1 tekrar, ±%10 kaydırma, 5 farklı rastgele tohum. Çakışma modeli kötümser: aynı alıcıda üst üste binen iki paket ikisini de bozar, capture etkisi yok. Bütün linkler kusursuz kabul edildi (zayıf sinyal kaybı yok).

| # | Senaryo | ACK başarısı* | Maks. backlog | Kesinti sonrası boşalma | Relay duty | Gateway duty | Tasma dinleme |
|---|---|---:|---:|---:|---:|---:|---:|
| S1 | 1 tasma direkt, SF11 | %100 | 1 | – | %0 | %0,2 | 7,6 s/sa (bugün 40) |
| S2 | 1 tasma, sadece relay, SF11 | %100 | 1 | – | %0,6 | %0,2 | 8,6 s/sa |
| S3 | 1 tasma, ikisini de duyuyor, SF11 | %100 | 1 | – | **%0** | %0,2 | 7,6 s/sa |
| S4 | 1 tasma relay, 6 sa kesinti, SF11 | %100 | 25 | 111 dk | %1,8 | %0,6 | 9,6 s/sa |
| S5 | 10 tasma relay, SF11 | %70 | 6 | – | %6,4 | %2,2 | 9,4 s/sa |
| S6 | 30 karışık, 6 sa kesinti, SF11 | %48 | 68 | **erimedi** | %10,7 ⚠ | %8,5 | 10 s/sa |
| S7 | S6, SF9 | %82 | 27 | 387 dk | %3,9 | %3,8 | 5,0 s/sa |
| S7c | S6, SF9 + CAD | **%88** | 29 | **285 dk** | %4,5 | %4,1 | 4,8 s/sa |
| S8 | 30 tasma tek relay, SF11 | %31 | 57 | – | **%19,4 ⚠** | %6,9 | 10,3 s/sa |
| S9c | 30 tasma tek relay, SF9 + CAD | %83 | 5 | – | %4,0 | %1,4 | 4,1 s/sa |
| S10 | S7c, hayvanlar duruyor (1 sa aralık) | %98 | 7 | 223 dk | %1,4 | %1,2 | 1,1 s/sa (bugün 15) |

\* Gateway açıkken yapılan denemeler. Kaçan ACK verinin kaybolduğu anlamına gelmez: **hiçbir senaryoda kapasite kaybı (728 kayıt) olmadı.**

**Okuma notları:**
1. Tek tasma ve küçük sürüde v0 kuralları tam çalışıyor. Relay yolu dahil kaçan ACK yok.
2. Asıl darboğaz kural değil, **SF11'in airtime'ı**. 30 tasmada SF11 ile hiçbir kural seti yasal duty içinde kalmıyor. SF9 ile hepsi rahat.
3. Tasmanın dinleme süresi bugünkü 10 s pencereye göre yaklaşık **5–14 kat** azalıyor (az trafikte 5 kat, backlog'lu senaryolarda daha fazla). Akım ölçülmeden pil ömrü iddiası yapılmaz.
4. ACK başarısının %100 olmaması tasarım hatası değil, tek kanallı paylaşımlı radyonun doğasıdır. Önemli olan, kaçanların veri kaybetmeden sonraki turda gitmesidir.

---

## 4. Bilinçli olarak v0 dışında bırakılanlar (v1 ve sonrası)

- Batch ACK (SF5K): yalnız simülatör ve ölçüm gerekli gösterirse.
- Çoklu atlama, birden fazla relay arasında seçim.
- Downlink COMMAND randevusu. Öneri: ACK'e "bekleyen komut var" biti eklenir, tracker ikinci bir slot açar. v0 slotunda **sadece ACK** gider.
- Link başına SF/ADR, çoklu kanal.
- Gateway'in uyanma zamanı atadığı (grant) zamanlama.

---

## 5. Doğrulama sırası

1. **Sahada SF ölçümü:** 1 tasma + gateway, Tekeler ve otlak noktaları, SF8–11 için RSSI/SNR/paket kaybı. Bu tek ölçüm R9'u kapatır ve en çok şeyi değiştirir.
2. **Masada slot testi:** 1 tasma, 1 relay, 1 gateway. Tasma ile gateway arasını zayıflatarak (anteni sökerek ya da mesafeyle) sadece-relay topolojisi kurulur. Her TX/RX başlangıç ve bitişi, slot zamanı ve ACK geliş anı loglanır.
3. Log alanları simülatörle aynı tutulur. Gerçek ölçüm simülatörden saparsa simülatör düzeltilir ve kurallar yeniden çalıştırılır.

---

## 6. Simülatörün sınırları (dürüst liste)

- Linkler kusursuz kabul edildi: zayıf sinyal, sönümlenme (fading) ve link bazlı paket kaybı yok. Gerçekte başarı oranı daha düşük olur.
- Çakışma modeli kötümser: capture etkisi yok. Gerçekte bazı çakışmalardan güçlü paket sağ çıkar.
- CAD ideal kabul edildi: duyulan her paket algılanıyor.
- Saat kayması, gateway'in Edge gecikmesi (>0,5 s), EVENT/COMMAND trafiği ve pil/akım modellenmedi.
- Topoloji sabit: hayvan hareket ettikçe linklerin değişmesi yok.

Çalıştırma:

```text
python3 tools/mac_sim/orun_mac_sim.py --selftest            # airtime'ı SF5B tablosuyla doğrula
python3 tools/mac_sim/orun_mac_sim.py                       # standart senaryo tablosu
python3 tools/mac_sim/orun_mac_sim.py --scenario S7c --set n_max=8 --set sf=8
```

---

## 7. Geliştirme ajanları için kural

- R1–R10 donduğunda, **bu kuralları değiştiren her öneri simülatör çıktısıyla gelir** (hangi senaryo, öncesi ve sonrası).
- Simülatörle ölçülebilecek bir soru için yeni "blocker" bölümü yazılmaz; önce simülatöre senaryo eklenir.
- Simülatörün modellemediği bir risk için önce simülatöre eklenip eklenemeyeceği sorulur.
