# ORUN v1 — kapsam ve sıra

**Durum:** Sahibi onayladı, 1 Kasım 2026. Önceki 150 maddelik taslak kapsamın yerine geçer.
Bu listede olmayan iş v1 değildir; yapılmaz, önerilmez, "ileride lazım olur" diye hazırlığı yapılmaz.

## Çalışma kuralı

1. Her adım **tek PR** olur ve **aynı gün main'e girer** ya da kapatılır. Birleşmemiş dalın üstüne yeni dal açılmaz.
2. Her adım, sahibin **uygulamada ya da cihazda göreceği** bir değişiklikle biter. "Hazırlık", "preflight", "shadow", "prova" adımı açılmaz.
3. Test, değişen davranış için yazılır ve son hal için bir kez koşulur. Aynı kodu tekrar tekrar doğrulamak ilerleme sayılmaz.
4. Bir adım bitince bu belgedeki durumu güncellenir. Yeni mimari belgesi açılmaz.

## Uygulama ilkeleri

- Uygulama yalnız gerçekten çalışanı gösterir. "Planlı", "demo" etiketi yok. Tanıtım sürüsü 2026-11-10'da kaldırıldı; örnek sürü yalnız testlerde (`edge/app/test/sampleHerd.ts`).
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
| 3a | Alanları tasmaya gönder (USB) | Her alanda "Hangi tasmalar": Hepsi ya da Seçerek (gruplar, tek tek hayvanlar); yeni alan önce Hepsi. Alan kaydedilince "Tasmalara şimdi gönder?" sorulur, sonra Alanlar → Tasmaya gönder'den de gönderilir. Tasma USB ile telefona bağlanır; o tasmanın tam alan listesi yazılır, tasma onaylayıp yeniden okununca hesaba kaydedilir. Her tasmanın durumu (güncel / eski / hiç gönderilmedi) Alanlar'da ve hayvanın kartında; tasmadaki alanların adları kartta Alanlar silinince (2026-11-11): tasmada eski alan kaldıysa "Tasmalardan sil" → işlem, tasma USB'ye bağlanınca `APP FENCE CLEAR` (Bluetooth'tan silme tasma yazılımında yok); canlı durumda tasmada hesapta olmayan alan görünürse "Tasmadaki alanları sil" (USB) | tamam (#239) |
| 3 | USB'den tüm ayarlar | Kabloyla her ayar değişir | rapor aralığı, hizmetler, alanlar var |
| 4 | BLE'den tüm ayarlar | USB, Bluetooth ve LoRa'da mümkün olan her iş aynı (sahip kararı 2026-11-01). Tasmaya dokununca bağlanır; ayar değiştirmeyi yalnız hesaba girmiş telefon yapar (anahtar tasma eklenirken verilen kökten türetilir, sahada internetsiz çalışır). Tasmanın Bluetooth'u üç yolla açılır: açılışta 10 dk (bugün), her rapordan sonra kısa süre, uzaktan LoRa komutuyla | sırayla: 4a okuma, 4b USB'den ayar, 4c Bluetooth'tan ayar + rapor sonrası açılma (tasma yazılımı), 4d kayıt çekme |
| 4a | Tasmanın canlı durumu | Tasmaya dokununca (Bağlantı listesi, hayvanın kartı) Bluetooth ya da USB ile bağlanır; aynı kart: pil, rapor aralığı, konum takibi, GPS, alanlar (hesaptaki kayıtla karşılaştırma), aktarma, kayıt hafızası, yazılım. Tanınmayan ORUN cihazı da okunur; gateway çıkarsa "Köprü olarak bağlan" | tamam (#240) |
| 4b | Cihazlar ve cihaz ayarları | Bağlantı sekmesi **Cihazlar** olur: üstte bu telefonun bağlantısı ve yakındakiler, altında bütün cihazlar türüne göre (Gateway'ler, Tasmalar; önce sorunlular), gruplar. Her cihazın ekranı: durum, bağlan (Bluetooth/USB), o cihazda olan ayarlar (rapor aralığı, konum takibi, aktarma, alanlar). Ayar hesaba kaydedilir; cihaza bağlanınca bekleyen ayarlar ve alanlar sormadan yazılır (şimdilik USB), her ayarın yanında cihazdaki değer ve "bekliyor / cihazda". Toplu ayar: hepsi / grup / seçerek. Hayvanın kartında tasmanın özeti ve "Tasma ekranı" | tamam (#241) |
| 4e | İşlemler ve tek alan akışı | (ORUN · Saha prototipinden) Her ayar kaydı ve "Tasmalara uygula" bir **işlem**dir; tasma başına durum: Sırada → (Gateway'de, yalnız LoRa) → Tasmaya işlendi / Zaten güncel / Reddedildi; yenisi gelirse eskisi "Yerine yenisi geldi". Alan: çiz → kaydet (tasmayı değiştirmez) → Hangi tasmalar → Tasmalara uygula; tasmaya bağlanınca (şimdilik USB) sıradakiler sormadan işlenir. İşlemler Cihazlar'da ve cihaz ekranında | tamam (#242; uygulamadan önce seçim: #243) |
| Y1 | Tek komutla yazılım yükleme | PC'de `bash tools/orun_cihaz_yukle.sh`: takılı cihazı tanır (tasma/gateway), durumunu okur, doğru yazılımı derler ve USB'den yükler, sonra aynı cihaz mı, anahtarı (kimlik/dönem), gateway izni ve kayıtları yerinde mi diye yeniden okur; silme komutu yok. Gateway PC Edge'e bağlıysa PC Edge yükleme boyunca durur, sonunda başlar. Yazılım sürümü derlendiği git sürümünü taşır ("0.5-7b7c540"), uygulamada canlı kartta görünür. Derleme ve yüklemede ilerleme çubuğu | tamam (#244–#247) |
| Y2 | Ürün yazılımı | Masa deneme yazılımı ve içindeki açık deneme anahtarı kalkar; tek tasma, tek gateway yazılımı; gateway deneme anahtarlı kaydı kabul etmez | iki adım (sahip kararı 2026-11-03): Y2a, Y2b |
| Y2a | Tek ORUN yazılımı | Her cihaza aynı yazılım (`rak4630_orun`; sahip kararı 2026-11-03); masa (bench) ve faz 2 yazılımları kalktı. Cihaz rolünü (tasma / gateway) açılışta kendi belleğinden anlar: tasma anahtarı ya da tasma kayıtları → tasma; gateway izni ya da gateway kayıtları → gateway; boş cihaz kaydedilmiş seçime göre, seçim yoksa **kurulmamış** kalır ve hiçbir şey yazmaz. Kurulmuş cihazın rolü ayarla değişmez. Tasmaya deneme kimliği yazılamaz; gateway yalnız sunucunun imzalı izniyle kayıt alır, deneme anahtarlı tasmayı reddeder. Kurulumda rol sorusu: boş cihaz USB'ye takılınca (Sürü → "+" ya da Cihazlar → "USB ile bağlı cihaza bağlan") "Tasma mı, gateway mi?" sorulur, GPS takılıysa tasma, yoksa gateway önerilir; seçim cihaza yazılır, cihaz o rolde yeniden başlar, uygulama yeniden bağlanır ve kurulum sürer (tasma → hesaba ekleme, gateway → köprü). PC'de de: `orun_cihaz_kontrol.py rol` | tamam (#248); 3 cihaz sahada yeni yazılımda, roller, anahtarlar ve kayıtlar korundu |
| Y2b | Teslim onayı anahtarı | Gateway kayıtları PC Edge'e ya da telefona verince gelen "aldım" onayı (EDGE ACCEPT) bugün açık deneme anahtarıyla doğrulanıyor. Her gateway'in kendi anahtarı olur: yalnız kabloyla PC'ye verilir, sunucu hesaptaki telefonlara dağıtır (önerilen yol, 2026-11-03). Bugün çalışır; deneme anahtarı yalnız teslim onayında | sırada: sahaya çıkmadan önce, 8'den önce (sahip kararı 2026-11-04) |
| 4c | Bluetooth'tan ayar ve alan | 4b ve 3a Bluetooth'tan da (hesaptaki telefon); her rapordan sonra kısa Bluetooth; Bluetooth ile yazılım güncelleme (USB + Bluetooth, sahip kararı); dururken GPS kapalı: son konumdan beri toplam hareket eşiği geçmezse (varsayılan 1 dk, ayarlanabilir) GPS açılmaz, son iyi konum yaşıyla gider; en geç rapor aralığının 2 katı, en az 1 saatte bir yeni konum; alan dışında ya da hareket algılayıcı yoksa her raporda konum | parçalar (sahip kararı 2026-11-04): 4c-1a → 4c-1b → 4c-2 → telefondan yazılım güncelleme (önce USB, sonra Bluetooth) → dururken GPS kapalı |
| 4c-1a | Rapordan sonra Bluetooth | Cihaz ekranında yeni ayar "Rapordan sonra Bluetooth": Kapalı, 15 sn, 30 sn, 1 dk, 2 dk, 5 dk (varsayılan 30 sn). Tasma her rapordan sonra bu kadar süre Bluetooth'tan bağlanılabilir olur; bağlanan telefon çıkınca açılıştaki gibi 10 dk daha açık kalır. Ayar tasmada saklanır (yeniden başlayınca kalır) ve hesaba kaydedilir; bugün USB'den yazılır, Bluetooth'tan 4c-1b'de, LoRa'dan 6'da (sahip kararı: ayara bağlı, üç yoldan aynı) | tamam (#249) |
| 4c-1b | Bluetooth'tan ayar yazma | Tasmaya Bluetooth'tan bağlanınca sıradaki ayarlar (rapor aralığı, konum takibi/aktarma, rapordan sonra Bluetooth) USB'deki gibi sormadan yazılır; "Tasmaya işlendi" yalnız tasmanın imzalı cevabıyla. Yalnız hesaba girmiş telefon yazar: sunucu her tasma için yazma anahtarı verir (tasmanın kökünden türetilir, kök değil), telefon saklar, sahada internetsiz çalışır, hesaptan çıkınca silinir. Her yazmada tasma yeni, tek kullanımlık bir soru verir; eski yazma tekrar edilemez. Gateway Bluetooth'tan ayar almaz. Sınır: kaybolan bir telefondaki anahtar, tasmanın anahtarı yenilenene kadar geçerli kalır | tamam (#250, `docs/milestones/4C1B.md`) |
| 4c-2 | Bluetooth'tan alanlar | "Tasmalara uygula" Bluetooth'tan da: tasmaya Bluetooth'tan bağlanınca (hayvan kartı, Cihazlar ya da "Tasmalara uygula" listesinde tasmaya dokununca) sıradaki alan listesi ayarlardan sonra sormadan yazılır; tasma listenin tamamını alır, bir imzalı onayla kaydeder, "Tasmaya işlendi" yalnız o onayla. Yalnız hesaba girmiş telefon (4c-1b ile aynı anahtar) | tamam (#251, `docs/milestones/4C2.md`) |
| 4d | Kayıt çekme | Gateway'e ulaşmamış kayıtlar tasmadan USB ya da Bluetooth ile. Tasmada (ya da yolda) bekleyen sayısı tasmaya bağlanmadan da görünür: ulaşan kayıtların sıra numarasındaki boşluklar (U1, Aktarım → Tasmalar) | sırada |
| U1 | Ekran düzeni (sahip isteği 2026-11-11) | **Bağlantı:** üstte yalnız bağlıyken "Bağlı" (hangi cihaza Bluetooth/USB ile; dokununca cihazın ekranı ve ayarları), boş "Bu telefon" kartı yok; Bluetooth/USB ve yakındaki cihazlar; Aktarım sağlığı: tek cümle + dört halka (Tasmalar → Gateway'ler → Köprü (Edge) → Sunucu): tasmadan kayıt geliyor mu, kaç kaydı henüz ulaşmadı (sıra numarası boşlukları, konumsuz kayıtlar dahil) ve bellekteki kayıt (son okumasıyla), gateway'de bekleyen (ölçüm zamanıyla) ve nasıl bağlı, Edge çalışıyor mu (son raporu ≤ 2 dk), sunucu çalışıyor mu; sunucu toplamı yok, sayaçlar Teknik ayrıntılar'da. "Bağlı"da USB ile okunan tasma da (son okuma zamanıyla); bekleyen işlemler tasma başına tek satır; Gateway'ler, Tasmalar. Tasmaya Bluetooth bağlantısı pencere kapanınca kopmaz, "Bağlantıyı kes"e kadar açık kalır (aynı anda tek tasma); gateway ekranında bellek, yeri, bağlantıyı kes. Gruplar Ayarlar'a, Toplu ayar kalktı (Ayarlar → Tasma ayarları). **Olaylar:** üstte "Her şey yolunda / N hayvana bakmalısın", Şimdi kartlarında işin düğmesi (Bul, Haritada, Tasma), altında saatli zaman çizelgesi (Hepsi/Hayvanlar/Sistem). **Sürü:** "Bugün, sürü" ortalamaları kalktı. **Hayvan kartı:** hareket hayvanın kendi günleriyle karşılaştırılır, en hareketli saat, bugün yürüdüğü; pil göstergesi, son 24 saat, gündüz şarjı, gidişat (tahmini kalan gün); tasma satırı. **Harita katmanları:** karo düğmeler, açık katmanın ayarı altında, uydu ayarları kapalı | tamam |
| 5 | Alan dışı bildirimi + LOST | İnek alandan çıkınca telefona bildirim; kayıp durumu | tasma alanı kendisi kontrol ediyor |
| 6 | LoRa'dan tüm ayarlar | Aynı işlemler sunucu → PC Edge → gateway → tasma; durum Sırada → Gateway'de → Tasmaya işlendi. **Fazladan telsiz trafiği yok** (sahip kararı 2026-11-02): komut tasmanın rapordan sonraki dinleme penceresinde gider, tasmanın onayı bir sonraki normal raporuna eklenir; "Gateway'de" bilgisi köprünün zaten gönderdiğinden gelir; "Tasmaya işlendi" yalnız tasmanın onayıyla, yol ne olursa olsun; Bluetooth'u uzaktan açma da bu yoldan. Sınırlar: tasma yalnız rapordan sonra ~10 sn dinler (komut en çok bir rapor aralığı bekler), paket başına 32 bayt (alan listesi ~17 paket), gateway'in sunucuya bağlı olması gerekir | yok |
| Y3 | Pil | İşlemci gerçekten uyur; pil ölçümü ayarlanır; tasmanın kaç gün gideceği uygulamada | uygulama tarafı var (2026-11-11): tasma ekranında pil kapasitesi (mAh, tasma başına, telefonda); "Pil ömrü": beklenen (üretici akımları + rapor aralığı; bugünkü yazılım / gerçek uykuyla) ve ölçülen (sahadaki günlük düşüş), güneş katılmaz. Tasma tarafı sırada (6'dan sonra) |
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

Uygulama düzeni (sahip kararı 2026-11-10, Meshtastic düzeni; 2 Kasım'daki Harita · Sürü · Alanlar · Olaylar · Cihazlar'ın yerine): **Harita · Sürü · Olaylar · Bağlantı · Ayarlar**. Alanlar ayrı sekme değildir, haritadan açılır. Bağlantı: bu telefonun gateway bağlantısı ve yakındaki cihazlar. Ayarlar: uygulama ayarları ve (sıradaki adımda) cihaz seçip kategorilerle cihaz ayarları, en altta tanılama. İlk adım #262: sekme çubuğu, Ayarlar sekmesi, Alanlar haritadaki düğmeden; cihaz listesi ve ayarları şimdilik Bağlantı'da. Ayarlar hayvanın
değil cihazın ayarıdır; tasma, gateway, ileride sensör ve vana aynı cihaz ekranını kullanır, ekranda yalnız o
cihazda olan donanım/hizmetin bölümleri görünür. Alanlar yalnız meralardır (sensör/vana gelince "Yerler"e
dönmez); sabit cihazlar Cihazlar'da ve haritada.

Bitirme sırası (sahip kararı 2026-11-02; Y2b 2026-11-04'te sahaya çıkmadan önceye alındı): 4b → Y1 → Y2a → 4c → 5 → 6 → Y3 → 4d → 7 → Y2b → 8 → 9.
Aktarma (relay) v1'de: tepelik arazide gateway'i görmeyen yerler için sahada üç cihazla denenip açılır (8. adımla).

## Sıra dışı düzeltmeler

- #253: tasmaların aynı anda yeniden denemesi cihaz başına gecikmeyle ayrıldı.
- #256: `APP FENCE DIAG?` — alan belleğinin A/B sayfalarını USB'den yalnız okur. 141A8887'de alan belleği bozulmuştu (9 Kasım), tek seferlik onarımla düzeltildi (#255, birleştirilmedi); **kök neden bilinmiyor**, tekrar ederse bu komutla bakılır.

## v1 sonrası

DIY sabit gateway, sensör (nem/sıcaklık, tank), vana/pompa, kişi/araç takibi, mesajlaşma, yardımcı kullanıcılar, hayvan defteri/fotoğraf, PDF rapor, yer işaretleri, uygulama kilidi.

## Uygulama düzeni (sahip kararı 2026-11-01)

Tek bir uygulamayı kopyalamıyoruz; bize uyan parçaları alıyoruz:
Bağlantı sekmesi ve durum renkli simge (Meshtastic), eşitleme düğmesi yok
(Tractive, Nofence), tasma ekleme Sürü'de "+" (Nofence), tasmanın bütün
bilgisi ve ileride ayarları hayvanın kartında (Weenect), alan kaydı = tasmaya
gönder (Nofence, 3. adım), kapsama haritada ve "buraya gateway koy" önerisi
(MeshCore, 8. adım), kısa ve tamamen Türkçe Ayarlar.
