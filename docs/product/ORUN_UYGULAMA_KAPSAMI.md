# ORUN v1 — kapsam ve sıra

**Durum:** Sahibi onayladı, 1 Kasım 2026. Önceki 150 maddelik taslak kapsamın yerine geçer.
Bu listede olmayan iş v1 değildir; yapılmaz, önerilmez, "ileride lazım olur" diye hazırlığı yapılmaz.

## Çalışma kuralı

1. Her adım **tek PR** olur ve **aynı gün main'e girer** ya da kapatılır. Birleşmemiş dalın üstüne yeni dal açılmaz.
2. Her adım, sahibin **uygulamada ya da cihazda göreceği** bir değişiklikle biter. "Hazırlık", "preflight", "shadow", "prova" adımı açılmaz.
3. Test, değişen davranış için yazılır ve son hal için bir kez koşulur. Aynı kodu tekrar tekrar doğrulamak ilerleme sayılmaz.
4. Bir adım bitince bu belgedeki durumu güncellenir. Yeni mimari belgesi açılmaz.

## Uygulama ilkeleri

- Uygulama yalnız gerçekten çalışanı gösterir. "Planlı", "demo" etiketi yok.
- Uydurma bilgi yok. Her değerin kaynağı var.
- "Şimdi nerede" = ölçüm zamanı en yeni kayıt. Bayat konum asla canlı gösterilmez.
- İnternetsiz çalışır, internet gelince kendiliğinden eşitler. Kullanıcı eşitleme için düğmeye basmak zorunda kalmaz.
- Sade Türkçe. Teknik ayrıntı yalnız "Teknik ayrıntılar" altında.
- Tek gateway: sabit/gezer yalnızca kurulum bilgisidir, ayrı mod ya da yazılım değildir.

## v1 sırası

| # | Adım | Sahibin göreceği | Durum |
|---|---|---|---|
| 0 | Toparlama | main = çalışan sistem, tek dal, bu liste repoda | tamam (#228) |
| 1 | Otomatik eşitleme | Uygulamayı açınca her tasmanın son konumu kendiliğinden gelir; geçmiş arka planda tamamlanır | tamam (#229) |
| 1a | Tek sunucu | PC'de üç servis yerine tek `orun-sunucu`; PC Edge aynı adrese yükler | tamam (#230, #231) |
| 1b | Kalıcı giriş | Kullanıcı adı ya da e-postayla giriş; uygulama kapanıp açılınca oturum açık | tamam (#232) |
| 1c | Gateway izni kendiliğinden | Tasma eklenince gateway'in izni köprü (PC Edge) üzerinden kendiliğinden güncellenir; `orun_provision.py gateway` adımı yok | tamam (#233) |
| 1d | Tasma çiftliğin bütün gateway'lerine | Tasma eklenirken çiftliğin her gateway'ine (en fazla 4) tek seferde yetkilendirilir; köprüye bağlanan gateway çiftliğe kendiliğinden girer. Sonradan eklenen gateway için tasmada "Cihaz ekle" bir kez daha (6. adımdan sonra LoRa'dan) | tamam (#234) |
| 1e | Uygulamadan "Cihaz ekle" | Ağ → Cihaz ekle: tasma telefona USB ile bağlanır, tek düğme; PC aracı yok | tamam (#234) |
| 2 | İnternetsiz harita | Ayarlar → İnternetsiz harita → İndir: Aydın–Muğla haritası (yol, köy, dere, OpenStreetMap, z15 + büyütme) telefona iner; internet yokken uydu/arazi yerine kendiliğinden gösterilir (internetsiz arazi/uydu yok, sahip kararı), sokak haritası hep bunu kullanır. PC: `tools/orun_harita.sh` | tamam (#235) |
| 2b | İnternetsiz uydu | — | gerek yok (sahip kararı 2026-11-01): uydu yalnız internetle; internetsizde telefondaki harita |
| 2c | Uygulama düzeni | 5 sekme: Harita · Sürü · Alanlar · Olaylar · Bağlantı (simgesi veri geliyor mu diye yeşil/sarı/kırmızı). Bağlantı her ORUN cihazı için: üstte sunucu satırı, "Bu telefon" kartı (bağlı cihaz ya da "Cihaz seçilmedi"), Bluetooth'ta yakındaki cihazlar listesi, USB'de gateway → köprü / tasma → kartı; altta diğer gateway'ler (nasıl bağlı, son görülme, bellek, kaç tasma). Tasma ekleme Sürü'de "+"; tasma bilgisi hayvanın kartında; taslak bölümler yok; tek ad: Gateway, Alan, Tasma | tamam (#236) |
| 2d | Cihaz adları | Ad hesapta saklanır (bütün telefonlarda aynı); tasmanın adı = hayvanın adı; ad ilk eklemede sorulur (tasma eklenince, adsız gateway ilk görülünce), sonra karttan değişir; adsız cihaz Bluetooth adıyla görünür (ORUN-XXXXXXXX). İnternetsiz verilen ad telefonda durur, internet gelince gönderilir | tamam (#237) |
| 3a | Alanları tasmaya gönder (USB) | Her alanda "Hangi tasmalar": Hepsi ya da Seçerek (gruplar, tek tek hayvanlar); yeni alan önce Hepsi. Alan kaydedilince "Tasmalara şimdi gönder?" sorulur, sonra Alanlar → Tasmaya gönder'den de gönderilir. Tasma USB ile telefona bağlanır; o tasmanın tam alan listesi yazılır, tasma onaylayıp yeniden okununca hesaba kaydedilir. Her tasmanın durumu (güncel / eski / hiç gönderilmedi) Alanlar'da ve hayvanın kartında; tasmadaki alanların adları kartta | tamam (#239) |
| 3 | USB'den tüm ayarlar | Kabloyla her ayar değişir | rapor aralığı, hizmetler, alanlar var |
| 4 | BLE'den tüm ayarlar | USB, Bluetooth ve LoRa'da mümkün olan her iş aynı (sahip kararı 2026-11-01). Tasmaya dokununca bağlanır; ayar değiştirmeyi yalnız hesaba girmiş telefon yapar (anahtar tasma eklenirken verilen kökten türetilir, sahada internetsiz çalışır). Tasmanın Bluetooth'u üç yolla açılır: açılışta 10 dk (bugün), her rapordan sonra kısa süre, uzaktan LoRa komutuyla | sırayla: 4a okuma, 4b USB'den ayar, 4c Bluetooth'tan ayar + rapor sonrası açılma (tasma yazılımı), 4d kayıt çekme |
| 4a | Tasmanın canlı durumu | Tasmaya dokununca (Bağlantı listesi, hayvanın kartı) Bluetooth ya da USB ile bağlanır; aynı kart: pil, rapor aralığı, konum takibi, GPS, alanlar (hesaptaki kayıtla karşılaştırma), aktarma, kayıt hafızası, yazılım. Tanınmayan ORUN cihazı da okunur; gateway çıkarsa "Köprü olarak bağlan" | tamam (#240) |
| 4b | USB'den ayar | Canlı kartta (USB ile bağlıyken) rapor aralığı (1 dk – 12 gün) ve hizmetler (konum takibi, aktarma); tasma "yazıldı" demeden yazılmış sayılmaz, sonra kart yeniden okunur | bu PR |
| Y1 | Tek komutla yazılım yükleme | PC'de tek komut: takılı cihazın tasma mı gateway mi olduğunu bulur, doğru yazılımı yükler; kayıtlar ve anahtarlar silinmez, sonunda cihazı kontrol eder | sırada |
| Y2 | Ürün yazılımı | Masa deneme yazılımı ve içindeki açık deneme anahtarı kalkar; tek tasma, tek gateway yazılımı; gateway deneme anahtarlı kaydı kabul etmez | sırada |
| 4c | Bluetooth'tan ayar ve alan | 4b ve 3a Bluetooth'tan da (hesaptaki telefon); her rapordan sonra kısa Bluetooth; Bluetooth ile yazılım güncelleme (USB + Bluetooth, sahip kararı); dururken GPS kapalı: son konumdan beri toplam hareket eşiği geçmezse (varsayılan 1 dk, ayarlanabilir) GPS açılmaz, son iyi konum yaşıyla gider; en geç rapor aralığının 2 katı, en az 1 saatte bir yeni konum; alan dışında ya da hareket algılayıcı yoksa her raporda konum | sırada |
| 4d | Kayıt çekme | Gateway'e ulaşmamış kayıtlar tasmadan USB ya da Bluetooth ile | sırada |
| 5 | Alan dışı bildirimi + LOST | İnek alandan çıkınca telefona bildirim; kayıp durumu | tasma alanı kendisi kontrol ediyor |
| 6 | LoRa'dan tüm ayarlar | Aynı komutlar sunucu → PC Edge → gateway → tasma; "sırada → ulaştı → uygulandı" durumu; Bluetooth'u uzaktan açma da bu yoldan. Sınırlar: tasma yalnız rapordan sonra ~10 sn dinler (komut en çok bir rapor aralığı bekler), paket başına 32 bayt (alan listesi ~17 paket), gateway'in sunucuya bağlı olması gerekir | yok |
| Y3 | Pil | İşlemci gerçekten uyur; pil ölçümü ayarlanır; tasmanın kaç gün gideceği uygulamada | sırada (6'dan sonra) |
| 7 | Gateway'in gözetimsiz çalışması | Eski telefon dokunulmadan 7/24 çalışır, yeniden başlar, yeniden bağlanır | ön plan servisi var |
| 8 | Haberleşme sağlığı | Haritada bölge bazlı sinyal ve paket başarısı | temel var |
| 9 | Güneş paneli katkısı | Pil ekranında şarj durumu ve güneş katkısı | yok |

### Ayarlanabilecekler (3, 4, 6)

Rapor aralığı, alan dışı aralığı, alanlar ve hangilerinin açık olduğu, hizmetler (konum/röle/gateway), LOST ayarları, serbest otlatma, cihaz adı, BLE'yi açma, telsiz ayarları (frekans, SF, güç).

- Üç kapı (USB, BLE, LoRa) cihazdaki **aynı** ayar sahibini kullanır; üç ayrı ayar sistemi yazılmaz.
- LoRa'dan gelen ayar tasmanın bir sonraki raporunda uygulanır.
- Telsiz ayarı uzaktan değişip onay gelmezse cihaz eski ayara kendisi döner.

### Mimari (sahip kararı 2026-11-01)

Tasma → Gateway (internetli telefon ya da Linux köprüsü) → **tek ORUN sunucusu** → uygulama.
Cihazlar birbirine değil **hesaba (çiftliğe)** eklenir; tanıştırmayı sunucu kendiliğinden yapar.
Ortak "çiftlik anahtarı" YOK (güvenlik kararı: ele geçirilen tek gateway bütün tasmaları
etkilemesin). Onay anahtarı tasma+gateway başına kalır; sunucu her gateway'in imzalı iznini
hesaplar, köprü (PC Edge / telefon) gateway'e kendiliğinden yükler (1c–1e).

Bitirme sırası (sahip kararı 2026-11-02): 4b → Y1 → Y2 → 4c → 5 → 6 → Y3 → 4d → 7 → 8 → 9.
Aktarma (relay) v1'de: tepelik arazide gateway'i görmeyen yerler için sahada üç cihazla denenip açılır (8. adımla).

## v1 sonrası

DIY sabit gateway, sensör (nem/sıcaklık, tank), vana/pompa, kişi/araç takibi, mesajlaşma, yardımcı kullanıcılar, hayvan defteri/fotoğraf, PDF rapor, yer işaretleri, uygulama kilidi.

## Uygulama düzeni (sahip kararı 2026-11-01)

Tek bir uygulamayı kopyalamıyoruz; bize uyan parçaları alıyoruz:
Bağlantı sekmesi ve durum renkli simge (Meshtastic), eşitleme düğmesi yok
(Tractive, Nofence), tasma ekleme Sürü'de "+" (Nofence), tasmanın bütün
bilgisi ve ileride ayarları hayvanın kartında (Weenect), alan kaydı = tasmaya
gönder (Nofence, 3. adım), kapsama haritada ve "buraya gateway koy" önerisi
(MeshCore, 8. adım), kısa ve tamamen Türkçe Ayarlar.
