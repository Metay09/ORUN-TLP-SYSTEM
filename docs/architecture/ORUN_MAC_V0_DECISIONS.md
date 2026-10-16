# ORUN MAC v0 — Gönderim, ACK ve Relay Zamanlama Kararları

**Durum:** ONAYLANDI ve DONDURULDU — sahip (Metay), 2026-10-17. Wire değişiklikleri yön olarak onaylandı; uygulamadan önce güvenlik incelemesi (R11, madde 3) zorunludur.
**Tarih:** 2026-10-16, güncelleme 2026-10-17 (R11 kümülatif ACK, R12 KAYIP, R3/R9/R10 değişiklikleri)
**Kapsam:** Tracker → (isteğe bağlı tek relay) → Gateway yolunda veri gönderimi, custody ACK dönüşü, backlog boşaltma, çakışma ve duty kuralları.
**Değişmeyenler:** TLP v1 baytları ve SF5K0 §8.1 "relay sadece taşıyıcıdır" kararı.
**Onaylanan wire değişiklikleri:** R11 için `ACK_REQUESTED` biti ve kümülatif custody ACK çerçevesi (§1, R11 sonu). Güvenlik incelemesi bitip uygulanana kadar SF5B baytları aynen geçerlidir.
**Kanıt:** `tools/mac_sim/orun_mac_sim.py` (yalnız bilgisayarda çalışan zamanlama simülatörü). Fiziksel RF, akım veya mevzuat uyumu kanıtı DEĞİLDİR.

Bu doküman SF5K0 §9–10'da açık bırakılan soruları **karara bağlamak** için yazıldı. Oradaki tespitler doğrudur; eksik olan bir karar setiydi.

---

## 0. Tek cümle

> **Tasma her kaydı hemen gönderir, arada bir "neyi aldın?" diye sorar; gateway elindekinin tam listesini söyler. Cevap sabit bir zamanda gelir. Kaçan cevap veri kaybı değildir; bir sonraki cevap eksikleri söyler.**

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

  Varsayımlar: relay gecikmesi 1,2–4,2 s (bugünkü `relay_config.h`), gateway commit en fazla 0,5 s. Airtime değerleri SF5B §18 tablosuyla bire bir aynıdır (simülatör `--selftest`). R11'deki 64 B'lık kümülatif ACK ile de X değerleri değişmez (SF11'de X_min 9,72 s).
- Slot yalnızca **ACK isteyen** pakette açılır (R11). ACK istemeyen paketten sonra tasma hiç dinlemez.
- **w = 0,5 s** başlangıç değeridir ve donanımda ölçülecektir.

**ACK'i kim zamanlar?**
- **Direkt yol:** Gateway, tracker paketini bitirdiği an + X'te ACK'i gönderir.
- **Relay yolu:** Gateway ACK'i hazır olur olmaz relay'e gönderir. Relay ACK'i **tutar** ve kendi duyduğu tracker paketinin bitişi + X anında gönderir. Gateway'in tracker'ın zamanını bilmesi gerekmez.
- **Aşağı yönde rastgele relay gecikmesi YOKTUR.** SF5K0 §9.4'teki "10 s yetmez" sonucu, downlink'e de 1,2–4,2 s rastgele gecikme koyduğu için çıkıyordu. v0'da relay ACK'i tam slot anında attığı için SF11'de bile X = 10 s yetiyor. Ağır yükte (30 tasma, SF11) birkaç "relay geç kaldı" görüldü; X sahada ölçülerek ayarlanır.
- **Slot önceliği:** Relay ve gateway, rezerve edilmiş bir ACK slotuyla çakışacak hiçbir forward veya ACK gönderimini başlatmaz. Gönderim slottan sonraya kayar.

### R2 — Kaçan ACK kayıp değildir; hemen tekrar gönderilmez

- Kayıt, onu kapsayan geçerli bir custody ACK gelene kadar silinmez. Bu zaten mevcut sözleşmedir.
- **ACK kaçarsa kayıtlar hemen tekrar gönderilmez** (sahibin 2026-10-17 kararı). ACK kümülatif olduğu için (R11) bir sonraki ACK, gateway'in neyi aldığını eksiksiz söyler. Yalnızca gateway'in gerçekten almadığı kayıtlar tekrar gider.
- Tekrar gönderim **aynı ciphertext** ile yapılır (SF5B §10). Gateway `(device, seq)` ile dedupe eder.
- **İstisna:** acil olay (EVENT) için ACK kaçarsa hemen bir kez tekrar denenir.

### R3 — Ne zaman, ne kadar, hangi sırayla

- **Tek aralık:** Konum aralığı = gönderim aralığı. Ayrı bir "yükleme aralığı" yoktur (sahibin 2026-10-17 kararı). Tipik değerler 15 / 30 / 60 dk; 3–5 dk yalnızca ara sıra açılır.
- **Ne zaman:** Rapor zamanı = uyanma. Birikme ayrı bir uyanma yaratmaz.
- **Ne kadar:** Uyanma başına en fazla **8 kayıt**. Sıra:
  1. Bu uyanmanın canlı kaydı.
  2. Son ACK'in "eksik" dediği kayıtlar.
  3. Hiç gönderilmemiş birikmiş kayıtlar, en eskiden başlayarak ve yalnızca son ACK'in kapsayabileceği pencere içinde (R11, kayan pencere).
- **Grup içi aralık:** Bir uyanmadaki paketler sabit aralıkla gider: `RELAY_ALLOWED=0` ise 0,25 s; `RELAY_ALLOWED=1` ise `relay_gecikme_max + sarılı_paket_airtime + 0,25 s` (SF11'de yaklaşık 6,9 s). Böylece relay bir paketi iletirken sıradaki gelmez (yarım çift yönlü radyo). İlk paketten önce CAD yapılır, grup içinde yapılmaz.
- **ACK isteği:** Birden fazla kayıt gönderilen uyanmanın son paketi her zaman ACK ister (R11).
- **İkinci tur:** İlk turun ACK'i geldiyse ve hâlâ gönderilecek kayıt varsa aynı uyanmada ikinci 8'li tur. SF ≤ 9'da açık, SF10–11'de kapalı: SF11'de relay %10'u aşıyor (§3.1).
- **Önceki ACK isteği cevapsızsa** (gateway kapalı, menzil dışı ya da çakışma): tasma uyanmada yalnız tek kayıtla ACK ister, birikmiş kayıtları körlemesine göndermez (bounded probing). Cevap gelince normal sıraya döner.

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
- Hedef: sahadaki tüm linkler için güvenilir olan **en düşük SF** ve 6–10 dB pay.
- v0: tek kanal, **ağ genelinde tek SF**. Link başına SF/ADR yoktur.
  - Neden: gateway'deki SX1262 aynı anda **yalnız bir SF** dinler. LoRaWAN'daki tasma başına SF, bütün SF'leri aynı anda çözen çok kanallı gateway çipleri (SX1302 gibi) sayesinde mümkündür. Böyle bir gateway alınırsa tekrar değerlendirilir.
- **Bu, kararların en etkilisidir:**
  - 30 tasma tek relay üzerinden: SF11'de relay duty **%19,4** (sınırın neredeyse 2 katı), ACK başarısı %31, backlog erimiyor.
  - Aynısı SF9 ve CAD ile: relay duty **%4,0**, ACK başarısı %83, p95 gecikme 15 s.

### R10 — Duty ve öncelik

- Her verici (tasma, relay, gateway) kendi TX süresini **kayan 1 saatlik pencerede** sayar. Hedef 300 s, sert sınır 360 s (%10). Duty verici başına ve alt bant başına sayılır (§4).
- %8'e gelince düşük öncelikli trafik (birikme) durur. Canlı kayıt ve EVENT devam eder.
- Yasal uyum **duty sayacıyla** sağlanır, LBT+AFA ile değil. CAD (R8) yalnızca çakışma azaltır: ETSI'nin LBT tanımı enerji tabanlıdır, CAD ise LoRa preamble arar.
- İleride: ACK'e "sonraki ACK isteğini geciktir" türünde bir akış kontrolü alanı eklenebilir; böylece gateway sıkışınca tasmaları yavaşlatır.
- **Öncelik:** EVENT (sınır ihlali vb.) > canlı kayıt > backlog. EVENT de R1 slot kuralını kullanır.
- **Relay ve gateway kendi duty'sini kendisi uygular.** Tasma relay'in yükünü bilemez. Birikme boşalırken relay %10'a yaklaşıyor: 36 sa birikme, 30 tasma, SF11'de R11 ile %8,6; ikinci tur açıksa %11,6 (sınır aşılıyor). Sınıra gelen relay forward'ı düşürür; tasma kaydı saklar, bir sonraki ACK eksikliği bildirir ve kayıt sonra tekrar gider.

### R11 — Kümülatif ACK, N pakette bir (sahibin 2026-10-17 kararı)

**Fikir:** Normal kayıtlar her pakette ACK beklemez. Tasma ara sıra ACK ister. Gateway o tasmadan kalıcı olarak kaydettiği **her şeyi** tek ACK'te söyler. Önemli kayıtlar hemen ACK ister.

**ACK ne zaman istenir** (ilk sağlanan koşul):
- Son istekten beri **N paket** gönderildi (varsayılan **4**; 4 / 6 / 10 / 24 seçilebilir).
- Son temastan beri **T süre** geçti (varsayılan **2 saat**). T, bağlantının koptuğunun ve relay'e geçme ihtiyacının (R4) en geç ne zaman fark edileceğini belirler; uzatmanın kazancı çok küçüktür.
- Uyanmada birden fazla kayıt gönderildi (birikme veya eksik tamamlama).
- Önceki istek cevapsız kaldı: cevap gelene kadar her uyanmada istenir.
- Tasma **sınır dışında**: her pakette.
- **Acil olay (EVENT):** her zaman, hemen.

**ACK içeriği:**
```
"base'e kadar hepsi tamam" + base'ten sonraki 32 kayıt için bit listesi
örnek: base = 120, bitler 121–152: 1 1 0 1 1 1 1 1 0 0 …
→ 120'ye kadar hepsi ve 121, 122, 124–128 silinir; 123 eksik
```
- Gateway her tasma için "kesintisiz en yüksek sıra numarası + sonrasındaki 32 kaydın var/yok listesi"ni **kalıcı** tutar. Yeniden başlasa da bu bilgiyi kaybetmez.
- Yalnızca kalıcı olarak kaydedilmiş (durable) kayıtlar "var" sayılır.
- Bu yapı sıra dışı onayları da taşır. AGENTS.md'deki "tek bir kesintisiz sınır her zaman yetmez" kuralıyla uyumludur.

**Tasmanın yaptığı:**
- ACK'in kapsadığı kayıtları siler.
- İstek paketinden önce gönderilmiş ve bit penceresi içinde "yok" görünen kayıtları **eksik** işaretler. Bunlar sonraki uyanmada canlı kayıttan sonra gider.
- Pencerenin dışındaki kayıtlar eksik sayılmaz, bilinmez kabul edilir. Tasma hiç gönderilmemiş birikmiş kayıtları da pencereyi aşmadan gönderir (**kayan pencere**). Böylece pencere dışında kalan kayıtlar gereksiz yere tekrar gönderilmez.
- ACK gelmezse hiçbir şey tekrar göndermez (R2).

**Faydası:**
- Gateway ve relay ACK yükü N'e bölünür.
- Tasma yalnız ACK istediği pakette dinler; dinleme süresi 2,5–4 kat azalır.
- Kaçan ACK yük oluşturmaz.

**Wire değişiklikleri (sahip onayladı 2026-10-17; uygulamadan önce güvenlik incelemesi gerekir):**
1. **`ACK_REQUESTED` biti:** PRODUCT_SECURE `path_flags` içinde bit 2 önerilir (bugün bit 2–7 "reserved, unknown reject"). Bit AAD içinde olduğu için doğrulanır.
2. **Kümülatif custody ACK çerçevesi:** SF5B §12'deki tek-nesne ACK'ine (0x05, 56 B) ek olarak; içerik `base (4 B) + 32-bit bitmap`, yaklaşık 64 B.
3. **Güvenlik sorusu:** SF5B ACK'i her nesnenin tam SHA-256 özetine bağlıdır. Kümülatif ACK ise `(cihaz, incarnation, sıra aralığı)` onaylar. Bunun SF5B §12 / SF4 custody yetkisiyle nasıl bağdaşacağı (HMAC hangi baytları kapsar, incarnation değişimi, yeniden başlatma) uygulamadan önce incelenmelidir. SF5K yığınının (#134–#140) HMAC, yetki ve seçmeli bırakma işi bu çerçeve için yeniden kullanılabilir.

**Önceki R11 (emniyetli 4'lü toplu gönderim)** bu kuralla değiştirildi. Karşılaştırma `--compare-batch` ile hâlâ çalıştırılabilir.

### R12 — KAYIP: süre değil, kaçırılan temas sayısı (sahibin 2026-10-17 kararı)

- **KAYIP = sınır dışında + art arda K ACK isteği cevapsız** (varsayılan **K = 3**, ayarlanabilir).
- Dışarıdayken her paket ACK istediği için tespit süresi kendiliğinden rapor aralığına uyar. Tek bir çakışma alarm üretmez.
- **Dışarıdaki aralık:** normal aralığın üçte biri veya **en fazla 1 saat**, hangisi kısaysa (varsayılan; kapatılabilir). Böylece uzun aralıklarda da KAYIP birkaç saatte anlaşılır.

| Normal aralık | Dışarıdaki aralık | K = 3 ile KAYIP |
|---|---|---|
| 15 dk | 5 dk | 15 dk |
| 1 sa | 20 dk | 1 sa |
| 6 sa | 1 sa | 3 sa |
| 12 sa | 1 sa | 3 sa |

- Bu, `ORUN_GEOFENCE_OPERATIONAL_POLICY.md`'deki "dışarıda B/3" kuralına **1 saat üst sınırı** ekler ve AGENTS.md'deki "OUTSIDE + yaklaşık 3 saat temassızlık" taslağını **K kaçırılan temas** ile değiştirir. KAYIP modu henüz yazılmadığı için bozulan bir kod yoktur. Güvenilir KAYIP, kimliği doğrulanmış ACK'e bağlıdır (mevcut kural). İki dokümana bu karara işaret eden not eklendi; mevcut `B / 3` çalışma zamanı kodu değişmedi.

### Ayarlar (USB'den, ileride uygulamadan)

| Ayar | Varsayılan | Not |
|---|---|---|
| Konum / gönderim aralığı | mevcut ayar | tipik 15 / 30 / 60 dk |
| ACK her N pakette | 4 | 4 / 6 / 10 / 24 |
| ACK en geç | 2 sa | kopukluk ve relay'e geçiş en geç bu sürede fark edilir |
| Uyanma başına kayıt | 8 | |
| İkinci tur | SF ≤ 9'da açık, SF10–11'de kapalı | açılırsa birikme daha hızlı, relay yükü fazla |
| KAYIP için K | 3 | art arda cevapsız istek |
| Dışarıdaki en uzun aralık | 1 sa | kapatılabilir |
| SF | ölçümle | ağ genelinde tek SF (R9) |
| Acil olay ACK'i | her zaman | ayarlanamaz |

---

## 2. Sorulan soruların cevabı

| Soru | Cevap |
|---|---|
| Biriken paketler **ne zaman** gönderilecek? | Normal rapor uyanmasında, canlı kayıttan sonra (R3) |
| **Ne kadar** gönderilecek? | Uyanma başına en fazla 8 kayıt: canlı + eksikler + pencere içi birikmiş (R3) |
| Her pakete ACK mi? | Hayır: N pakette bir (varsayılan 4) veya en geç 2 saatte bir; acil olaylar ve sınır dışı her pakette (R11) |
| ACK kaçarsa gönderdiklerini tekrar atar mı? | Hayır. Sonraki ACK kümülatiftir; yalnız gateway'in almadıkları tekrar gider (R2, R11) |
| KAYIP ne zaman? | Sınır dışında art arda 3 cevapsız ACK isteği (R12) |
| Relay üzerinden aktarım nasıl? | Tracker `RELAY_ALLOWED` ile ister, relay iletir; ACK'i relay tutup slotta verir (R1, R4, R6) |
| Relay ve gateway aynı anda duyarsa? | Dedupe; tek ACK yolu, direkt öncelikli; sonraki gönderimde relay devre dışı (R4, R5) |
| ACK ne zaman gelecek? | Her zaman TX_bitişi + X'te; X tek ayar (R1) |
| Pencere kaçarsa? | Veri kaybı yok; sonraki ACK eksikleri söyler (R2) |
| Relay 2. paketi alırken 1.'yi gönderirse? | Olmaz: uyanmadaki paketler relay'in iletmesine yetecek sabit aralıkla gider (R3). Başka tasmanın paketi gelirse bu olağan bir çakışmadır; CAD azaltır, kümülatif ACK eksik kaydı yakalar (R8, R11) |

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

### 3.1 R11: her pakete ACK vs kümülatif ACK (`--compare-cum`)

30 tasma (10 direkt, 10 hem gateway hem relay, 10 yalnız relay), 24 sa, CAD açık, 3 tohum, en kötü saat:

| SF, aralık | Her pakete ACK: gateway / relay | N = 4: gateway / relay | Tasma dinleme (s/sa): önce → N = 4 | Tekrar gönderilen paket |
|---|---|---|---|---|
| SF11, 15 dk | %5,6 / %7,9 | %2,2 / %4,5 | 9,0 → 2,9 | %14 |
| SF11, 30 dk | %2,8 / %3,7 | %1,2 / %2,4 | 4,2 → 1,2 | %6,5 |
| SF11, 60 dk | %1,5 / %1,6 | %1,2 / %1,3 | 2,0 → 0,8 | %3,1 |
| SF9, 15 dk | %1,3 / %1,4 | %0,4 / %0,9 | 3,7 → 1,0 | %3,4 |
| SF9, 30 dk | %0,6 / %0,7 | %0,3 / %0,5 | 1,8 → 0,5 | %1,5 |
| SF9, 60 dk | %0,3 / %0,4 | %0,3 / %0,3 | 0,9 → 0,3 | %0,4 |

Birikme: 36 sa birikmiş kayıt (her tasmada 72), 30 dk aralık, aynı 30 tasma, 48 sa:

| SF | Her pakete ACK, 8/uyanma | Kümülatif N = 4, 1 tur | Kümülatif N = 4, 2 tur |
|---|---|---|---|
| SF11 | 21–27 sa; gateway %10,5 ⚠, relay %10,8 ⚠ | **15–16,5 sa**; gateway %2,3, relay %8,6 | 11–12,5 sa; relay %11,6 ⚠ |
| SF9 | 9–13,5 sa; %4,3 / %4,2 | **8–11,4 sa**; %0,6 / %2,8 | **5,3–6,6 sa**; %1,1 / %4,6 |

**Okuma:**
- Kümülatif ACK, 15–30 dk aralıkta gateway yükünü 2–3 kat düşürüyor; 60 dk'da fark küçük, çünkü orada zaten 2 saatte bir ACK isteniyor.
- Birikme SF11'de yaklaşık 1,5 kat daha hızlı eriyor ve her vericide %10'un altında kalıyor. Eski yöntemde ise gateway ve relay sınırı aşıyordu.
- SF9'da 1 turda kazanç küçük; 2 tur açıkken birikme yaklaşık 2 kat hızlı eriyor ve relay %4,6'da kalıyor. **Öneri: SF9 veya altında 2 tur açık, SF10–11'de kapalı.**
- Simülatör relay'in duty kesmesini (R10) modellemiyor; sınıra yaklaşan senaryolarda gerçek boşalma biraz daha yavaş olur.
- Tekrar gönderilen paketler, gateway'in gerçekten almadığı (çakışmada kaybolmuş) paketlerdir. Kaçan ACK yüzünden tekrar gönderim olmaz.
- "Sunucuda olmayan kayıt" her senaryoda gün sonunda birkaç kayıttır (yeni üretilmiş veya yolda olanlar). Onay bekleyen ama sunucuya ulaşmış kayıtlar bundan ayrıdır ve kayıp sayılmaz.

---

## 4. BTK / ETSI sınırları (869,525 MHz için)

| Alt bant | Azami güç | Duty | Not |
|---|---|---|---|
| **869,4–869,65 MHz** (ORUN) | **500 mW e.r.p.** | **≤ %10** | veya LBT+AFA; BTK Teknik Ölçütler 2022 Tablo 1 satır 21, ETSI EN 300 220-2 bant O |
| 868,0–868,6 MHz | 25 mW e.r.p. | ≤ %1 | veya LBT+AFA; ikinci bant adayı (relay → gateway) |
| 869,7–870 MHz | 5 mW | sınırsız | çok düşük güç |

- Duty **verici başına** (tasma, relay ve gateway ayrı ayrı) ve **alt bant başına** sayılır; gözlem penceresi 1 saattir. Gateway'in ACK'leri ve relay'in iletimleri kendi %10'larına sayılır.
- SX1262 en fazla 22 dBm (yaklaşık 158 mW) verir; 500 mW sınırı sorun değil. Anten kazancıyla e.r.p. hesabı belgelenmelidir.
- ⚠️ BTK sayfasında 2026'da yüklenmiş görünen yeni bir PDF açılamadı. Sahaya çıkmadan 863–870 MHz satırları güncel metinden kontrol edilmeli.

---

## 5. Bilinçli olarak v0 dışında bırakılanlar (v1 ve sonrası)

- Birden çok kaydı tek pakete koyma (kayıt başına airtime −%46). Yeni bir paket tipi gerektirir.
- Relay → gateway bacağını düşük SF'ye veya ikinci alt banda taşıma (araştırma önerisi; ölçüm sonrası).
- Çoklu atlama, birden fazla relay arasında seçim.
- Downlink COMMAND randevusu. Öneri: ACK'e "bekleyen komut var" biti eklenir, tracker ikinci bir slot açar. v0 slotunda **sadece ACK** gider.
- Link başına SF/ADR, çoklu kanal (çok kanallı gateway gerektirir).
- Gateway'in uyanma zamanı atadığı (grant) zamanlama.

---

## 6. Doğrulama sırası

1. **Sahada SF ölçümü:** 1 tasma + gateway, Tekeler ve otlak noktaları, SF8–11 için RSSI/SNR/paket kaybı. Bu tek ölçüm R9'u kapatır ve en çok şeyi değiştirir.
2. **Masada slot testi:** 1 tasma, 1 relay, 1 gateway. Tasma ile gateway arasını zayıflatarak (anteni sökerek ya da mesafeyle) sadece-relay topolojisi kurulur. Her TX/RX başlangıç ve bitişi, slot zamanı ve ACK geliş anı loglanır.
3. Log alanları simülatörle aynı tutulur. Gerçek ölçüm simülatörden saparsa simülatör düzeltilir ve kurallar yeniden çalıştırılır.

---

## 7. Simülatörün sınırları (dürüst liste)

- Linkler kusursuz kabul edildi: zayıf sinyal, sönümlenme (fading) ve link bazlı paket kaybı yok. Gerçekte başarı oranı daha düşük olur.
- Çakışma modeli kötümser: capture etkisi yok. Gerçekte bazı çakışmalardan güçlü paket sağ çıkar.
- CAD ideal kabul edildi: duyulan her paket algılanıyor.
- Saat kayması, gateway'in Edge gecikmesi (>0,5 s), EVENT/COMMAND trafiği ve pil/akım modellenmedi.
- Topoloji sabit: hayvan hareket ettikçe linklerin değişmesi yok.
- R10'daki relay/gateway duty kesmesi ve R12'deki KAYIP durumu modellenmedi; sınır dışı davranışı yok.

Çalıştırma:

```text
python3 tools/mac_sim/orun_mac_sim.py --selftest            # airtime'ı SF5B tablosuyla doğrula
python3 tools/mac_sim/orun_mac_sim.py                       # standart senaryo tablosu
python3 tools/mac_sim/orun_mac_sim.py --scenario S7c --set n_max=8 --set sf=8
python3 tools/mac_sim/orun_mac_sim.py --compare-cum         # R11: her pakete ACK / kümülatif ACK
python3 tools/mac_sim/orun_mac_sim.py --compare-batch       # eski emniyetli toplu gönderim karşılaştırması
```

---

## 8. Geliştirme ajanları için kural

- R1–R12 donduğunda, **bu kuralları değiştiren her öneri simülatör çıktısıyla gelir** (hangi senaryo, öncesi ve sonrası).
- Simülatörle ölçülebilecek bir soru için yeni "blocker" bölümü yazılmaz; önce simülatöre senaryo eklenir.
- Simülatörün modellemediği bir risk için önce simülatöre eklenip eklenemeyeceği sorulur.
