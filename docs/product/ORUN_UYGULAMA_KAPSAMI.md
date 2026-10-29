# ORUN uygulaması — tam kapsam

**Ne bu belge:** ORUN telefon (ve web) uygulamasının **bütün** kapsamı, tek listede. Uygulama bu listeye göre tasarlanır ve kodlanır; "sonra bunu da yapmamız gerekiyordu, unutmuşuz" olmasın diye her şey burada.

**Durum:** Taslak, sahibin onayını bekliyor. 26 Ekim 2026.

**Dayandığı kaynaklar:**
- Sahibin istekleri (sohbetler).
- 9 Ekim'deki `orun.html` denemesi (v1–v6).
- ChatGPT'nin UX v1 belgesi (#173).
- Proje belgeleri:
  - `PRD.md`
  - `ORUN_PRODUCT_SYSTEM_ARCHITECTURE.md`
  - `ORUN_GEOFENCE_OPERATIONAL_POLICY.md`
  - `ORUN_FIELD_NETWORK_DIAGNOSTICS_PLAN.md`
  - `ORUN_GATEWAY_COMMAND_AUTHORITY_DIRECTION.md`
  - `ORUN_APP_ENTITY_MESSAGING_DIRECTION.md`
  - `ORUN_APPLICATION_TRANSPORT_SURFACE.md`
  - `ORUN_FEATURE_AND_STATUS_INVENTORY.md`
  - `ORUN_STEP3_REAL_SECURITY_PLAN.md`

## 29 Ekim 2026 — Sahibin yeni kesinleştirdiği gereksinim ve sıra

**Ortak veri, tek Edge'e/telefona kapanmaz.** Bir telefonun oluşturduğu alan,
aynı saha/cihazlara erişim yetkisi olan diğer telefon ve bilgisayarlarda, backend
ulaşılabiliyorsa, otomatik görünür. İnternet yokken yerel kalıcı taslakla
çalışılır; bağlantı döndüğünde idempotent ve çatışma denetimli eşitleme yapılır.
Aynı kural hayvan profilleri, gruplar, atamalar, notlar, gözlemler, gerçek
olaylar, hedef planları ve işlem sonuçları için de geçerlidir.
Kişisel UI tercihleri, cihaz bağlantıları/çevrimdışı harita dosyaları ve
güvenlik anahtarları aynı shared-data kategorisinde değildir.

**Alan uygulama USB + BLE + LoRa olmalıdır.** Yakında USB veya BLE ile doğrudan
Tracker'a; uzakta yetkili Gateway üzerinden LoRa'ya. Relay yalnız paketleri
iletir. Aynı tasmada tek GeofenceMutationOwner / CAS / durable RESULT korunur.
Bir alanın başka telefonda görünmesi, ona yetki olması veya Gateway'e teslim
edilmesi tasmanın o alanı **uyguladığı** anlamına gelmez.

**30 günlük alan simülasyonu iptal edilmiştir.** `§6`'daki eski "Kaydetmeden
önce" simülasyon satırı ve `§15` paket 2'deki "deneme" artık yapılacak iş
değildir. Kontrol/test amacıyla farklı simülasyonlar, sahibin ayrıca
istemesi halinde ayrılır; canlı ürün ekranına "geçmiş 30 gün uyarısı" özelliği
konulmaz.

**Geliştirme önceliği:** Gerçek cihaz/telefon yetkisi (3C), kanonik sunucu
paylaşılan veri deposu, Android offline sync, web/PC görünürlüğü; ardından
yetkili BLE/USB gerçek geofence aktarımı ve güvenli LoRa uzak aktarımı.
İlgili tek uygulama sözleşmesi:
`docs/architecture/ORUN_SHARED_DATA_SYNC_AND_GEOFENCE_DELIVERY.md`.
Eski `§15` paket numaraları tarihsel taslağıdır; güncel bağımlılık/öncelik
bu satır ve yeni sözleşme ile belirlenir.

---

## Uygulamanın kuralları

1. **Bitmiş ürün gibi görünür.** Ekranlarda "planlı", "demo", "yol haritası" etiketi olmaz. Bir bilgi o cihazdan henüz gelmiyorsa, o yerde sakin ve doğru bir boş durum yazar. Örnek: "Bu tasma hareket özeti göndermiyor".
2. **Uydurma bilgi yok.** Gösterilen her değerin bir kaynağı vardır. Örnek veri yalnız *Tanıtım sürüsü* modunda vardır ve bu açıkça belirtilir.
3. **Zaman dürüstlüğü:**
   - "Şimdi nerede" sorusunun cevabı, **ölçüm zamanı** en yeni olan kayıttır.
   - Geç gelen kayıt yalnız geçmişi uzatır.
   - Gecikmeli gelmek, bağlantı olmadığının kanıtı değildir.
   - Bayat konum asla "canlı" gösterilmez.
4. **Teslim dürüstlüğü:**
   - Teslim zincirinin halkaları ayrıdır: tasmada kalıcı, istasyonda kalıcı, telefonda kalıcı, sunucuda doğrulandı.
   - "Gönderildi", "ulaştı" demek değildir. "Ulaştı" da "uygulandı" demek değildir.
5. **Tek ORUN cihazı.** Kullanıcı "tasma/röle/BASE" türü seçmez. Bir cihaz vardır; onun donanımı vardır ve açık hizmetleri vardır: konum, aktivite, sensör, röle, istasyon, vana. *Hayvana takılı*, *sabit* ve *araçta* yalnız kurulum bilgisidir.
6. **Çevrimdışı önce.** İnternetsiz dağda harita, sürü, uyarılar ve kayıtlar çalışır. İnternet gelince sessizce eşitlenir.
7. **Sade Türkçe:** "Az önce görüldü", "2 saattir sessiz". Teknik terimler yalnız *Teknik ayrıntılar* altında.

İşaretler: her maddenin yanında, verinin nereden geldiği yazar.
- ✅ bugün var
- 📱 yalnız uygulama işi
- 🖥 sunucu işi
- 📡 tasma/istasyon yazılımı gerekir

---

## 1. Kullanıcılar ve anlar

- **Sahip:**
  - sabah/akşam kontrol;
  - sahada hayvanı bulma;
  - uyarıya tepki;
  - cihaz kurma ve bakım;
  - alan çizme.
- **Yardımcı (aile, çoban):** izleme ve uyarı alma. Yetkisi sınırlı: alan veya cihaz değiştiremez. 🖥 (3C sonrası)
- **Sabit istasyon telefonu:** şarjda duran eski telefon. Ekranı kapalı, 24 saat istasyondan alır ve sunucuya gönderir.
- **Bilgisayar (web):** büyük ekranda harita, rapor ve dışa aktarma. 🖥 (sunucu haritası bugün ✅, temel)

## 2. Çalışma biçimleri

- **Cep (gezici istasyon):**
  - Bluetooth/USB ile istasyona bağlanır ve arka planda çalışır. ✅
  - Yakındaki istasyonu kendisi bulup bağlanır. 📱
- **Sabit istasyon modu:**
  - "Bu telefonu istasyon yap" seçeneği. 📱
  - Ekran kilitli çalışır, şarj ve bağlantı izlenir. 📱
  - Sağlık bilgisi sunucuya gider. 🖥
  - Kesintide ve yeniden açılışta kendisi toparlanır. 📱
- **Tanıtım sürüsü:** cihaz yokken uygulamayı örnek sürüyle göstermek. Açıkça "örnek" yazar. 📱
- **Saha kaydı:** bilgisayardaki USB kaydından veri içe almak; test ve servis için. 📱

## 3. Gezinme

Alt sekmeler: **Harita · Sürü · Alanlar · Olaylar · Ağ**. Sağ üstteki profil düğmesi **Ayarlar**'ı açar. **"+"** düğmesi bulunduğu sekmede doğru şeyi ekler: Sürü'de cihaz, Alanlar'da alan. Android geri tuşu her ekranda öngörülebilir çalışır. ✅ (temeli var)

## 4. Harita

**Altlık:**
- Uydu: zoom 18'e kadar net, hesap/anahtar gerektirmeyen ve lisansı uygun bir kaynak. 📱
- Arazi/topo, karanlık harita. 📱
- **3B rölyef**: eğik bakış, yükselti verisi. 📱

**İnternetsiz harita (📱 + paket üretimi 🖥):**
- Aydın + Muğla vektör harita.
- Mera çevresinde zoom 18–19 uydu paketi.
- İndirme yöneticisi: alanı çizme, boyut tahmini, ilerleme, güncelleme, silme.
- İndirilmemiş bölgede "bu bölge indirilmedi" yazar, ama hayvanlar ve izler yine çizilir.

**Katmanlar** (her biri aç/kapa):
- **Hayvanlar:** tazelik halkası, durum rengi, yoğun yerde kümelenme. ✅
- **İz:**
  - tarih aralığı seçimi ✅
  - zaman kaydırıcıyla oynatma 📱
  - hız/sıçrama süzgeci 📱
- **Yoğunluk (ısı) haritası:** en çok gidilen yerler. 📱
- **Alanlar:** dolgu yok / hafif / çizgili; kenar ölçüleri. Hayvanları ve ısı haritasını kapatmaz. 📱
- **İstasyon ve röleler,** bağlantı hatları. ✅/📱
- **Kapsama:** 5 m hücreler; güçlü / zayıf / yalnız röleyle / veri yok. Üst üste ölçümlerin ortalaması alınır. "Veri yok", "kapsama yok" demek değildir. 📱 (🖥 toplu)
- **Yer işaretleri:** su, kapı, tuzluk, barınak, tehlike, arıza. 📱 (🖥 paylaşım)
- **Telefonun konumu.** Hayvanın konumuyla asla karışmaz. 📱

**Araçlar:**
- hayvan ara;
- konumum;
- bütün sürüyü göster;
- mesafe/alan ölç;
- **Bul** (pusula: uzaklık, yön, yükseklik farkı). Yükseklik, kayıttaki irtifadan gelir ✅;
- yol tarifi (telefonun harita uygulaması). 📱

## 5. Sürü (varlıklar)

**Liste:**
- Sıralama: dikkat isteyen önce (alarm → kayıp → sessiz → eskiyen → düşük pil → iyi). 📱
- Süzgeç: grup/mera. 📱
- Arama: ad, küpe no, tasma. 📱
- Durum özeti cümlesi. 📱

**Hayvan profili:**
- **Kimlik:** ad, küpe no, ırk, cinsiyet, doğum, renk, fotoğraf. 📱 (🖥 paylaşım)
- **Tasma ataması** geçerlilik zamanıyla. Tasma başka hayvana geçince eski kayıtlar eski hayvanda kalır. 📱🖥
- **Konum:**
  - son konum, tazelik, ölçüm kalitesi (uydu, hdop) ✅
  - ölçüm ve geliş saati ✅
  - yer adı 📱
- **Durum:** alan içi/dışı (tasma doğrular) 📡, kayıp 📱🖥, serbest otlatma 📱.
- **Aktivite:**
  - günlük ve haftalık hareket süresi ✅
  - otlama/dinlenme/yürüme ayrımı: tasma bunu kayda koyunca 📡
  - yürünen yol 📱
- **Pil:**
  - seviye ✅
  - güneş panelinin katkısı ve şarj durumu 📡
  - kalan süre tahmini: veri biriktikçe 📱
- **Defter:** sağlık, doğum, aşı, tedavi notları ve fotoğraf. 📱 (🖥 paylaşım)
- **Konum iste:** adım adım durum (sırada → istasyonda → tasmaya ulaştı → konum geldi / ulaşılamadı). 📡 (dönüş yolu)
- **Gruplar:** oluşturma, toplu seçme (haritada kement), toplu işlem (alan ata, aralık değiştir). 📱 (+📡 uygulama)

## 6. Alanlar (sanal çit)

**Çizim (📱):**
- Nişangâh modu: harita parmağın altında kayar, merkezdeki artı nokta ekler.
- Köşe sürükleme, kenar ortasından nokta ekleme, geri al/yinele.
- Canlı ölçü: hektar, dekar, kenar uzunlukları.
- **Yürüyerek sınır alma:** çit boyunca telefon GPS'i.
- **İzlerden öneri:** hayvanların gezdiği yerden sınır önerisi.
- GeoJSON/KML içe ve dışa aktarma.

**Doğrulama (📱, tasma kurallarıyla birebir):**
- en fazla 8 alan ve toplam 64 köşe;
- kendini kesen çizgi yok;
- kapasite çubuğu ve "sadeleştir".

**Kaydetmeden önce (📱):**
- 30 günlük geriye dönük alan uyarısı simülasyonu yapılmaz (sahip kararı).
- Köşe ölü bölgedeyse kapsama uyarısı.

**Uygulama:**
- Tek tasmaya, gruba ya da hepsine.
- Tasma başına sonuç: gönderildi → ulaştı → uygulandı / reddedildi / belirsiz.
- Kaldırma ayrı ve onaylı bir işlem. 📡 (güvenli yazma + dönüş yolu)

**Kurallar:**
- Alan dışındayken rapor sıklaşır (taban aralığın 1/3'ü). 📡
- Giriş/çıkış ancak art arda doğrulanmış ölçümlerle kabul edilir. 📡
- **Serbest otlatma** (2 / 6 / 12 saat): alan uyarıları susar, kayıp uyarısı sürer. 📱 (+📡)
- Hayvan başına hangi alanların açık olduğu seçilir. 📱📡

## 7. Olaylar ve bildirimler

**Olay türleri:**
- **Alan dışına çıktı / geri döndü:** tasmanın doğruladığı. 📡
- **Kayıp:** rapor aralığına bağlı; kaç rapor kaçırıldığına göre. 📱🖥
- **Uzun süre sessiz:** "sırtın arkası istasyonu görmüyor olabilir" gibi olası sebep. 📱
- **Pil:** azalıyor / kritik. ✅
- **Hareketsizlik veya az hareket.** 📱
- **Tasma düşmüş olabilir.** 📡
- **Cihaz arızası:** uydu alıcısı, sensör, depolama. 📡
- **İstasyon:** çevrimdışı / dolmak üzere ✅; sunucuya ulaşılamıyor ✅.

**Yaşam döngüsü:**
- Aktif → güncellendi → çözüldü. Çözülmeyi cihazdan gelen yeni ölçüm yapar. 📡
- "Gördüm" yalnızca kullanıcı işaretidir, olayı kapatmaz. 📱
- Her olayda ölçüm zamanı ve iletim zamanı ayrı yazar. ✅

**Bildirim (📱):**
- İnternetsiz de çalışır: telefon istasyona bağlıyken yerel bildirim.
- Sessiz saatler (alan dışı yine bildirilir), alarm sesi.
- Kime gideceği seçilir. 🖥

**Defter:** günlere ayrılmış zaman çizelgesi; uyarı/not/cihaz süzgeci. ✅ (temeli var)

## 8. Ağ ve cihazlar

**Ağ özeti:**
- Veri yolu: tasmalar → istasyon → telefon → sunucu.
- Her halkada bekleyen kayıt sayısı. ✅

**Cihaz listesi:**
- Tek cihaz modeli: donanım/yetenek, **istenen ve etkin hizmet**, engel nedeni. 📡 (bugün kısmi ✅)
- Kurulum bilgisi: hayvana takılı / sabit / araçta. 📱

**Cihaz ayrıntısı:**
- Pil ve güneş. ✅/📡
- Sinyal: RSSI/SNR; doğrudan mı, röleyle mi. ✅
- Son haberleşme ✅, cihazda bekleyen kayıt 📡.
- Yetki paketi sürümü ✅, yazılım sürümü ✅.
- **Tanılama** (📡 sağlık modeli):
  - anlık durum;
  - sayaçlar;
  - son önemli olaylar;
  - anahtarsız tanılama raporu dışa aktarma. 📱

**Ayarlar (komut):**
- Ne değiştirilir: rapor aralığı (15 dk / 30 dk / 1 saat; kısa süreli sık mod), hizmet aç/kapa, ad.
- Nereden değiştirilir:
  - yakındaysa Bluetooth/USB ile 📡;
  - uzaktaysa istasyon üzerinden 📡 (dönüş yolu).
- Komut durumu: taslak → kuyrukta → iletildi → doğrulandı → uygulandı / reddedildi / belirsiz.

**Diğer:**
- **Kapsama ve röle yeri önerisi:** zayıf bölgeler; röleyle görülen yerler. 📱🖥
- **Yazılım güncelleme:** tasma ve istasyon yazılımını telefondan Bluetooth/USB ile güncelleme. 📡📱 (güncelleme depolama kararı bekliyor)
- **İstasyon yönetimi:**
  - doluluk ✅;
  - imzalı yetki paketinin otomatik güncellenmesi 📱 (3C);
  - sabit/gezer bilgisi 📱.

## 9. Kurulum ve sahiplik

**İlk açılış (📱):**
- Hoş geldin.
- Sunucu: QR ya da adres.
- **Telefon kaydı:** tek seferlik kodla onay. 🖥 (3C)
- İzinler, her biri nedenini söyleyerek: Bluetooth, konum, bildirim.
- İnternetsiz harita önerisi.
- Tanıtım sürüsü.

**Yeni cihaz (📱 + 3C):**
1. USB-OTG ile tak.
2. Cihaz tanınır.
3. **Ne için?** hayvan / sensör / vana / istasyon / röle.
4. Hizmetler.
5. Hayvana ata: ad, küpe, renk.
6. Rapor aralığı.
7. Alan.
8. **Anahtar kapalı zarfla yazılır.** Telefon anahtarı göremez.
9. İstasyon yetkisi kendiliğinden güncellenir.
10. **İlk gerçek konum** istasyona ulaşmadan "tamamlandı" denmez.

**Cihaz yaşamı (📱🖥📡):**
- başka hayvana taşıma;
- yeniden anahtarlama (yeni epoch);
- emekliye ayırma;
- fabrika sıfırlaması.

## 10. Hesap, kullanıcılar, yetki

- **Sahip; yardımcılar** (izleme / sınırlı). Davet, iptal. 🖥 (3C)
- **Onaylı telefonlar listesi.** Kayıp telefonu iptal etme. Okuma anahtarı riski sahibin kararıyla (D1). 🖥
- **Kişi konumu** (ileride kişi takibi): açık izin ve görünürlük ayarı olmadan gösterilmez. 🖥

## 11. Veri, eşitleme, güvenilirlik

- **Telefon defteri:** kayıt silinmez ✅.
  - Sunucuya gönderim kuyruğu, çakışma/red gösterimi, tekrar deneme ✅.
  - Saat kalitesi belirsizse kesin yaş iddia edilmez. 📱
- **Dışa aktarma:**
  - CSV / GeoJSON / KML 📱
  - günlük/aylık sürü raporu (PDF) 📱🖥
- **Yedekleme ve telefon değiştirme:** ad, not ve alanların sunucuda tutulması 🖥; telefondan dışa/içe alma 📱.
- **Çok telefon:** aynı kayıt iki telefondan gelirse tek sayılır ✅.

## 12. Gelecek cihaz türleri

Aynı iskelete takılır; ayrı uygulama yapılmaz.

- **Sensör** (nem/sıcaklık, tank seviyesi): zaman serisi grafiği, eşik uyarısı. 📡
- **Vana/pompa:**
  - komut + fiziksel durum + onay;
  - "iletildi", "açıldı" demek değildir;
  - yetki ve tekrar saldırısı korumasıyla. 📡🖥
- **Kişi/araç takibi:** gizlilik kurallarıyla. 📡🖥
- **Mesajlaşma:** mesaj kimliği, teslim durumu; okundu bilgisi yok. 📡🖥

Haritada bunların hepsi ayrı işaret ve süzgeçle görünür. Her türün kendi ayrıntı sayfası vardır.

## 13. Profesyonel uygulama gereksinimleri

**Arka plan ve pil:**
- Ön plan servisiyle bağlantı ✅.
- Telefon pilini koruma, Android pil kısıtlarına dayanıklılık. 📱

**Erişilebilirlik:**
- en az 48 dp dokunma;
- yeterli kontrast;
- ekran okuyucu etiketleri;
- **Güneş modu**: daha koyu yazı, kalın işaret;
- koyu tema (telefona göre). 📱

**Dil:** Türkçe. Bütün metinler tek sözlükte durur, ileride İngilizce eklenebilir. 📱

**Durumlar:** her ekranda boş / yükleniyor / hata / çevrimdışı durumu tanımlı. 📱

**Güvenlik:**
- Anahtar ve gizli bilgi günlüğe yazılmaz ✅.
- İsteğe bağlı uygulama kilidi (parmak izi / PIN). 📱
- Bluetooth eşleşmesi yetki sayılmaz. 📡 (3D)

**Güncelleme:** uygulama sürüm kontrolü ve sürüm notları. 📱

**Ölçek:**
- 100+ hayvan, 100.000+ kayıtta akıcı: kümeleme, sayfalama, arka planda hesap. 📱
- Hedef RF alanı başına ~100 cihaz.

**Kalite:**
- birim testleri;
- uçtan uca testler (gerçek defter + sunucu);
- ekran görüntüsü karşılaştırması;
- CI. 📱

## 14. Kod mimarisi

Geliştirilebilir olması için:

- **Modüller:** `harita`, `suru`, `alanlar`, `olaylar`, `ag`, `kurulum`, `ayarlar`. Her biri kendi ekranları ve kendi testleriyle.
- **Alan modeli** (tek yerde, belgelerdeki isimlerle):
  - Cihaz, Hizmet, Varlık (hayvan/kişi/araç/yer);
  - Atama (geçerlilik zamanlı);
  - Alan, Gözlem (konum/telemetri/aktivite);
  - Olay (yaşam döngülü), Komut/Sonuç;
  - İstasyon, Kullanıcı/Telefon.
- **Veri katmanı:**
  - telefon defteri (SQLite);
  - eşitleme (sunucu);
  - bağlantılar (Bluetooth/USB/istasyon).
  - Ekranlar yalnız alan modelini görür; nereden geldiğini bilmez. Yeni bir cihaz türü ya da bağlantı geldiğinde ekran değişmez.
- **Tasarım sistemi:** renkler, yazı, ölçü ve bileşenler tek yerde. Bileşen örnekleri: tazelik halkası, durum rozeti, hücre, panel, pusula. Her ekran aynı dili konuşur.
- **Harita motoru:** MapLibre GL. 3B rölyef, vektör ve uydu, PMTiles ile internetsiz harita. Leaflet'ten geçiş.
- **Değiştirilmeyen çekirdek:** bugünkü `edge/app/src/core` (defter, teslim, açma, gönderim, anahtarlar). Testleri aynen kalır.

## 15. Yapım sırası

| # | Paket | İçerik | Bağımlılık |
|---|---|---|---|
| 1 | **İskelet + tasarım sistemi** | 5 sekme, ayarlar, tema/güneş modu, sözlük, durumlar; MapLibre harita, 3B, katmanlar; bugünkü gerçek verilerle Harita/Sürü/Olaylar/Ağ, Bul, iz oynatma, yoğunluk, kapsama | yok (📱) |
| 2 | **Alanlar (telefonda)** | çizim, doğrulama, çevrimdışı taslak, hedef seçimi, kapsama uyarısı, yer işaretleri; 30 günlük simülasyon yok | yok (📱) |
| 3 | **İnternetsiz harita** | paketler, indirme yöneticisi | paket üretimi (🖥) |
| 4 | **3C: telefon kaydı + kurulum** | kod onayı, kapalı zarf, yeni cihaz akışı, istasyon yetkisi otomatik, yardımcı kullanıcılar | 🖥📡 |
| 5 | **Olay ve kayıp** | tasmanın alan olayları, kayıp, bildirimler | 📡 olay paketi |
| 6 | **Komutlar** | rapor aralığı, alan uygulama, konum iste, komut durumu | 📡 dönüş yolu |
| 7 | **Sabit istasyon modu + sağlık** | eski telefonla 24 saat istasyon | 📱🖥 |
| 8 | **Aktivite ve güneş** | otlama/dinlenme ayrımı, güneş katkısı | 📡 |
| 9 | **Yazılım güncelleme** | cihazlara telefondan güncelleme | 📡 |
| 10 | **Raporlar, dışa aktarma, yedek** | PDF/CSV, telefon değişimi | 📱🖥 |
| 11 | **Yeni cihaz türleri** | sensör, vana, kişi/araç, mesaj | 📡🖥 |

Her paket bittiğinde **bitmiş ürün** olarak kullanılabilir. Arkasından gelen paket yalnız yeni özellik ekler; önceki ekranlar yeniden yapılmaz.
