# ORUN Android — Ürün Deneyimi ve Ekran Tasarımı v1.0 (TASLAK)

**Tarih:** 25 Ekim 2026  
**Statü:** UX önerisi / onaylanmamış tasarım; mimari, güvenlik protokolü ve runtime değişikliklerini yetkilendirmez.  
**Referanslar:** `PRD.md`, `AGENTS.md`, `docs/architecture/ORUN_PRODUCT_SYSTEM_ARCHITECTURE.md`, `docs/architecture/ORUN_STEP3_REAL_SECURITY_PLAN.md`, `docs/milestones/SF5L7.md`, `SF5L8.md`, `STEP3A.md`, `STEP3B.md`; mevcut `edge/app/src/ui/App.tsx`.

## 1. Amaç ve ilkeler

ORUN uygulaması, kullanıcının **hangi hayvanın nerede olduğunu, bilginin ne zaman ölçüldüğünü, ne kadar güvenilir olduğunu, hangi cihazdan geçtiğini ve hangi aşamaya kalıcı olarak teslim edildiğini** anlamasını sağlar.

- **Çevrimdışı öncelik:** Önceden yetkilendirilmiş telefon, yerel veritabanı ve indirilmiş harita ile internet olmadan çalışır; yeni sahiplik/yetki üretemez.
- **Güncellik ≠ bağlantı:** `observed_at` esas alınır; son aktarım zamanı asla ölçüm zamanı yerine geçirilmez. Ekrandaki "Güncel" etiketi configurable sınır, gerçek gözlem zamanı ve saat kalitesine bağlıdır. Geç gelmiş bir kayıt güncel gösterilmez.
- **Teslim ≠ gönderim:** Cihazda kayıt, Gateway kalıcı custody, Edge kalıcı custody ve backend kabulü birbirinden ayrıdır.
- **Hayvan ≠ cihaz:** Hayvan/varlık kimliği ile takılan ORUN cihazı ayrı kayıtlar ve zaman sınırlı atamalardır.
- **Cihaz ≠ rol:** Cihazda tanınmış donanım yeteneği, kullanıcının istediği hizmet ve gerçekten etkinleşmiş hizmet bağımsızdır. Konum, sensör, röle ve Gateway birbirini dışlayan ürün tipleri değildir.
- **Güvenlik doğru konumlandırılır:** Kimlik bilgileri uygulama arayüzünde gösterilmez; anahtar oluşturma/cihaz provisioning işlemleri ve Gateway yetkisi ilgili güvenlik aşamalarına tabidir. Bluetooth bağı tek başına sahiplik değildir.
- **İddiaların dürüstlüğü:** Pilot, masa testi, plan, üretim entegre ve fiziksel yeterlilik birbirine karıştırılmaz.

## 2. Bilgi mimarisi

### Ana alt gezinme (5 sekme)

1. **Genel Bakış:** Güncel/eski/bilinmeyen konum sayıları, kritik uyarılar, sürü görünümü, Gateway/telefon/sunucu aktarım sağlığı, son olaylar.
2. **Harita:** Son geçerli konumlar, tarihsel rota, konum zamanı, mera poligonları, röle ve Gateway katmanları, telefonun konumu (izinliyse), çevrimdışı harita yönetimi.
3. **Hayvanlar:** Arama/filtreler, hayvan listesi, detay, hareket/aktivite, batarya, geofence, ölçüm kalitesi, cihaz ataması.
4. **Uyarılar:** Aktif, incelenmiş ve çözümlenmiş durumlar; alarm önceliği, yerel bildirim; olay kimliği ve oluşma/iletim zamanları.
5. **Cihazlar:** ORUN kimlikleri, yetenekler, istenen/uygulanan hizmetler, Gateway/röle, Bluetooth/USB, kalıcı kayıt, yetki ve tanılama.

### Üst sağ ayarlar

- Hesap/sahiplik, yetkilendirilmiş telefonlar, sunucu ve senkronizasyon, bildirim tercihleri.
- Çevrimdışı harita indirme/silme ve disk kota göstergesi.
- Görünüm, dil, gizlilik, veri saklama, uygulama sürümü, tanılama günlükleri (gizli anahtarsız).

## 3. Ekran sözleşmeleri

| Ekran | Birincil kullanıcı sorusu | Asıl bilgiler | Ana eylem |
|---|---|---|---|
| Genel Bakış | Kim güvende, kim kontrol gerektiriyor? | Son ölçüm yaşı, risk, teslim özeti | Hayvana / haritaya git |
| Harita | Hayvan en son nerede görüldü? | Koordinat, kaynağı, ölçüm zamanı, kalite, rota | Hayvan detayı / rota |
| Hayvan detayı | Bu hayvanın güncel ve geçmiş durumu nedir? | Kimlik, atama, batarya, aktivite, sınır, son gözlem, teslim | Haritada göster / profil |
| Uyarılar | Hangi olaya müdahale etmeliyim? | Olay tipi, gerçek oluşma zamanı, durum, güven | İncele / okundu işaretle |
| Cihaz detayı | Cihaz ne yapabilir, ne yapıyor? | Donanım, hizmet niyeti, etkin hizmet, engel nedeni, bağlantı | Tanılama / güvenli değişiklik |
| Sunucu/Edge | Veri nereye kadar güvenli ulaştı? | Bekleyen, kaydedilen, tekrar, çatışma, son doğrulanan senkronizasyon | Yeniden dene |

### Durumlar / içerik kalitesi

- Konum: `Güncel`, `Eski konum`, `Son bilinen konum`, `Henüz geçerli konum yok`, `Saat bilgisi belirsiz`.
- Ağa bağlantı: `Yerel bağlantı açık`, `Son temas`, `Aranıyor`, `Ulaşılamıyor`, `Bağlantı belirsiz`. Bunlar konum etiketlerinden ayrı gösterilir.
- Teslim: `Tasmada saklı`, `Gateway kalıcı olarak aldı`, `Telefonda kalıcı`, `Sunucuda doğrulandı`, `Belirsiz/yeniden denenecek`, `Çatışma/Reddedildi`.
- Cihaz hizmeti: `Destekleniyor`, `İstendi`, `Etkin`, `Engelli (neden)`, `Uygulama bekliyor`; kullanıcı isteği gerçek etki sayılmaz.
- Komut: `Taslak`, `Kuyrukta`, `İletildi`, `Kimliği doğrulandı`, `Uygulandı`, `Reddedildi`, `Belirsiz`. `TX_DONE` hiçbir zaman `Uygulandı` demek değildir.

## 4. İlk kurulum akışı (hedef)

1. **Yerel sistem / sunucu:** Sistemin sahibi, yerel yetki servisini veya uzaktaki mevcut ORUN sunucusunu tanımlar. Telefon internetsiz kullanılabilir ama ilk sahiplik ve anahtar yetkisi gerektiğinde yetkili servise erişim zorunlu olabilir.
2. **Telefon Edge kaydı (3C):** Telefon cihaz üstünde korunan Ed25519 anahtarı üretir, backend'de sahibin tek seferlik onayı ile kaydedilir; sadece kendisine ait hayvanları okuma ve teslim izni alır. Kök anahtar telefona veya Gateway'e gönderilmez.
3. **ORUN cihazı bulma:** BLE/USB ile yakındaki benzersiz donanım kimliği okunur. İlk tracker gizli anahtarının güvenli provisioning işlemi mevcut 3A temeline dayanır; nihai UI bu yetkili adımı yönlendirir ama atlamaz.
4. **Hizmetleri ayarlama:** Takip, sensör, faaliyet özeti, Röle, Gateway bağımsız seçilir. Donanım/flash kapasitesi/yetki/pil politikası nedeniyle engellenenler sebebiyle görüntülenir.
5. **Hayvan atama ve güvenli doğrulama:** Hayvan kimliği, tasma ilişkisi, rapor aralığı ve mera seçilir. İlk gerçek konum ve kayıt teslimi gözlenmeden kullanıcıya “tamamlandı” iddiası verilmez. Gerekli yetki/sunucu yoksa “kurulum bekliyor” sunulur.

**Kurulum sonrası:** Cihazı hayvandan başka bir hayvana taşımak varlık ataması değişikliğidir; gizli anahtar devretme veya geçmiş kaydı yeniden yazma işlemi değildir.

## 5. Harita davranışları

- Varsayılan: En son **geçerli** gözlem; ölçüm yaşı konum etiketinde görünür.
- Filtreler: Son konum / geçmiş rota / mera sınırı / Gateway ve röle / sinyal kapsamı (yeterli ölçüm varsa).
- Kayıt tekrar geldiğinde harita noktasını 'şimdi görüldü' diye güncellemez; `received_at` ayrı ayrıdır.
- Kritik konum hareketi veya mera dışına çıkış ölçümü kaynağına göre işlenir, sonradan sunucu tarafından tahminen yaratılmaz.
- İnternet olmadan indirilen karolar ve önceden yetkilendirilmiş yerel veriler kullanılır. Karolar yoksa koordinat/listeler yine erişilebilir, boş harita açıkça bildirilir.
- Telefonun kendi GPS'i hayvanın son bilinen GNSS konumu ile karıştırılmaz.

## 6. Alarm ve bildirimler

- Cihaz olayı kimlikli bir yaşam döngüsüne sahip: `Aktif`, `Güncellendi`, `Çözüldü`. Kullanıcının `Okundu` işareti ayrı, yalnız arayüz işlemidir.
- Olası olaylar: doğrulanmış sınır çıkışı, kritik batarya, GNSS arızası, uzamış veri sessizliği, depolama veya yetki hatası.
- Bilinen bağlantı kesintisiyle hayvanın tehlikede olması eş anlamlı değildir. Örneğin “son konum 2 saat önce” yazılır, otomatik “kayboldu” denmez.
- Öncelik: kritik alarm görünür ve gerektiğinde ilk güvenli radyo fırsatında gönderime alınır; normal verilerin toplu teslimi kritik sinyali geciktirmemeli.
- Alarm temizleme, gerçek cihazdan doğrulanmış olay kapanışı gerektirir; 'bildirimi kapat' bunu taklit etmez.

## 7. Gateway / Röle / Store-Forward

Kullanıcıya bir kayıt için denetlenebilir aktarma zinciri sunulmalı:

`Tracker kayıt kalıcı → [opsiyonel Röle iletim] → Gateway durable custody + doğrulanmış ACK → Telefon ledger COMMIT + doğrulanmış kabul → Sunucu verified ledger kabul`.

- Röle, iletme hizmetidir; Gateway'in yetkili custody ACK'sini kendisi üretemez.
- Aynı cihazda birden çok hizmet olabilir; fakat Gateway/Relay hizmetlerinin gerçek depolama, yetkilendirme ve RF/pil gereklilikleri sağlanmadan etkinliği gösterilmez.
- Gateway'in bağlantısı yoksa tasma kendi verilerini saklar. Gateway doluysa kabul edilmeyen veri tasma üzerinde kalır. Telefon ve sunucu yoksa Gateway'in kabul ettiği verilerin kalıcılığı ayrı sınırdır.
- Görsel göstergeler yüklenen nesne sayısı / tekrar / çakışma / son eşitleme / ayrıntılı hata gösterir; `0 bekleyen` yalnız ilgili katmanın durumu demektir.

## 8. İşlevsel olmayan gereksinimler

- Dokunma hedefleri en az 44×44 dp; ekran okuyucu etiketleri ve okunabilir kontrast.
- Düşük güç/bağlantı koşullarında arka plan servisinin gerçek Android davranışı fiziksel testle doğrulanmalı; işletim sisteminin zamanlayıcıları mutlak zaman garantisi vermez.
- Tekrar mesajları idempotent ele alınmalı. Aşama durumları ve yerel kuyruk uygulama kapatılıp açıldığında tutarlı olmalı.
- Kimlik bilgilerinin uygulama günlüklerine, dışa aktarılan tanılama raporuna veya açık metin sunucu yapılandırmasına dökülmesi önlenmeli.
- Kayıt çok olduğunda liste/harita performansı (yığın, sorgu, sayfalama) test edilmeli.
- Otomatik senkronizasyon mobil veri kullanım sınırları ve pil yönetimi ile uyumlu olmalı.
- Çevrimdışı harita paketleri sahadaki ihtiyaç alanına göre önceden indirilebilir, boyut/kota/sürüm net gösterilir.

## 9. Var olan kaynaklar ve geliştirme sınırları

**Mevcut prototipte:** React + TypeScript + Capacitor Android, BLE/USB Gateway erişimi, telefon SQLite ledger, Harita sekmesi, sunucuya kayıt gönderimi, bazı durum sayaçları ve loglar var. En son Step 3A gerçek Tracker anahtarlarının fiziksel testini belgeliyor; 3B Gateway'in imzalı kalıcı snapshot kod/testi var fakat fiziksel kontrol bekliyor.

**Henüz bitmiş deneme yapılmamış nihai UX:** yeni beş sekmeli gezinme, kurulum sihirbazı, varlık kayıt yöneticisi, çevrimdışı harita indirme, alarm yaşam döngüsü, 3C/3D telefon-BLE-yetki sıkılaştırması, tam röleli yol doğrulaması, güvenli uzaktan servis değişiklikleri ve üretim onayı.

**Uygulama önerisi:** `edge/app/src/core` katmanındaki doğrulanmış kayıt/teslim mantığını koru. `src/ui/App.tsx` içindeki iki sekmeli basit görünümü yeni sayfa/sunum bileşenlerine ayır. UI; bağlantı/veritabanı/crypto sahipliğini kendi içine çekmesin. Yeni kullanıcı/varlık verisi sunucu modelinde, yerel önbelleği Edge tarafında ayrı yönetilsin.

## 10. Önerilen teslim sırası

| Öncelik | Paket | Ön koşul / Kabul |
|---|---|---|
| P0 | Kullanıcıya dürüst durum: gözlem zamanı, teslim basamakları, tanılama | Var olan gerçek kayıtlar üzerinden yanlış 'canlı/teslim' gösterimi yok |
| P1 | Beş sekmeli kabuk, hayvan kayıtları, harita kartları, cihaz detayları | Mevcut BLE/USB/ledger/backend davranışı bozulmaz |
| P1 | 3B kalıcı Gateway izinleri, 3C telefon kaydı, 3D güvenlik | Fiziksel yetkilendirme, negatif test, rollback/power-fail |
| P2 | İlk kurulum akışı, güvenli ayar ekranları, cihaz atamaları | Kısmi kurulum ve yetki yok hataları doğru işlenir |
| P2 | Mera çizimi, offline harita indirimi, alarm yaşam döngüsü | Sensör/cihaz olayları ile eşleşir; çift alarm yok |
| P3 | Röleli saha görünümü, enerji/menzil ölçümü, kapsama analizi | Gerçek arazi, kayıp ve gecikmeli ACK kayıtları |
| P3 | Gelişmiş profil, çoklu kullanıcı, filo raporları, güncelleme UI | Her alt sistem için ayrı güvenlik/uygulanabilirlik kanıtı |

## 11. Kabul testleri (UI ve veri anlamı)

- `observed_at = 07:41`, `received_at = 09:40` → 09:40'ta gelen konum **eski** gösterilir.
- İnternet kesilir → telefon eski kayıtlarını açar, server 'bekliyor' görünür, sahte çevrimiçi etiketi yoktur.
- Gateway yeniden başlatılır → kayıt durumu doğrulanmadan yeşil 'teslim edildi' oluşmaz.
- Aynı ciphertext tekrar gelir → yerel kayıtta çift konum oluşturulmaz.
- Telefonda kayıt COMMIT hatası → Gateway kabul mesajı gönderilmez.
- Bir paket röle üzerinden ulaşır → Röle yetkili custody kaynağı olarak gösterilmez.
- Telefon 'okundu' seçer → hayvan cihazındaki aktif alarm kapanmaz.
- Yetkisiz telefon bağlanır → korunmuş kayıtları teslim aldırıp Gateway'e sildiremez.
- GNSS modülü olmayan cihazda konum servisi istenir → `Engelli: GNSS yok`, kullanıcı ayarı sessizce değişmez.
- Komut radyo üzerinden iletilir ama RESULT gelmez → `Uygulandı` gösterilmez.
- Geçersiz tarih/saat kalitesi → kesin canlılık yaşı iddia edilmez.
- Yeni cihaz yetkili ilk kayıt olmadan kalıcı Gateway ACK'si veremez.

## 12. Prototip

Etkileşimli HTML: `orun_android_ui_prototip.html`; tamamen çevrimdışı, üçüncü taraf kütüphanesiz, örnek veri ile çalışır. **APK değildir ve cihaz/sunucu işlemi gerçekleştirmez.**
