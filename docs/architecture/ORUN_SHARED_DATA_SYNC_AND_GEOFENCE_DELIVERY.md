# ORUN — Çok cihazlı ortak veri ve USB/BLE/LoRa alan uygulama sözleşmesi

**Durum:** Sahibin 29.10.2026 tarihli açık ürün gereksinimi; **mimari uygulama planı, kod/servis henüz uygulanmadı**.
**Simülasyon:** 30 günlük sanal çit geçmiş simülasyonu **kapsam dışı**.
**Öncelik:** Yetkili tüm telefonlar ve bilgisayarlar aynı ortak kaynakları görür; tasmaya uygulama sonucu bundan tamamen ayrı tutulur.

Bu tasarım mevcut onaylı yönleri tamamlar, onların yerini almaz:
- `ORUN_PRODUCT_SYSTEM_ARCHITECTURE.md` — Backend kanonik, Edge offline;
- `ORUN_APP_ENTITY_MESSAGING_DIRECTION.md` — Entity Registry ve çevrimdışı işlem çatışmaları;
- `ORUN_GEOFENCE_CONFIGURATION_DISTRIBUTION.md` — REPLACE_SET/CLEAR_SET, hedef başına RESULT, CAS;
- `ORUN_APPLICATION_TRANSPORT_SURFACE.md` — tek mutation owner, USB/BLE/LoRa adaptörleri;
- `ORUN_STEP3_REAL_SECURITY_PLAN.md` — 3C telefon kaydı, gerçek kullanıcı/cihaz yetkisi;
- `APP_ZONES_TARGETS_USB.md` — PR #192'de sadece telefona yerel alan/hedef taslağı ve fiziksel USB yardımcısı.

## 1. Kullanıcının göreceği davranış

1. Telefon A'da alan çizilip **kaydedildiğinde**, yerel taslak kalıcıdır; bağlantı ve yetki varsa sunucuya eşitlenir. İlgili alanın durumunda **Bu cihazda**, **Eşitleniyor**, **Senkronize**, **Çakışma / Müdahale gerekli** ayrımı görünür.
2. Telefon B ve bilgisayar, aynı *yetkili saha/hesap* altında alanı ve revizyonunu backend'den görür. Fiziksel konum, aynı Wi-Fi veya aynı Edge cihazına bağlı olmayı gerektirmez. Sunucuya erişim (örneğin Tailscale) gerekir.
3. Telefon B internetsizken bile daha önce eşitlenmiş alanı görür. Çevrimdışı değişiklikler telefonda kalıcı kuyrukta tutulur ve erişim geri geldiğinde yetki/çatışma denetiminden geçirilir. Sadece internet simgesi görmek eşitleme kanıtı değildir.
4. Bir alanın diğer ekranlarda görünmesi **tasmanın sınırı uyguladığı anlamına gelmez**. Uygulama talimatı ayrı bir kullanıcı eylemi ve ayrı durumu olan işlem kaydıdır.
5. Kullanıcı alanı **yakından USB**, **yakından BLE** veya **uzaktan Gateway üzerinden LoRa** yoluyla bir ya da çok tasmaya uygulayabilir. Operasyon birden çok kanaldan geçse bile hedef tasmadaki **aynı GeofenceMutationOwner** nihai kararı verir.
6. Birden fazla yetkili ekran aynı alanı düzenlerse kayıp güncelleme olmaz: eski revizyon, yeni alanı sessizce ezemez. Güvenlik açısından önemli geometri çakışmaları otomatik son-yazan-kazan ile çözülmez.
7. Diğer kullanıcının sadece görüntüleme yetkisi varsa sınırı görebilir, fakat değiştiremez veya tasmaya gönderemez. Yalnızca bir tasma için yetkili kullanıcı diğer sahaları/cihazları göremez.

## 2. Kanonik sahiplik: üç ayrı veri düzlemi

```text
                  TELEFON A           TELEFON B          WEB/PC
                 (yerel depo)        (yerel depo)         (önbellek)
                      \                  |                 /
                       +----- kimlikli senkronizasyon ----+
                                         |
                                  ORUN Backend
                       +----------------+-----------------+
                       |                |                 |
                 Paylaşılan veri     Gözlemler         Komut/sonuç
                (alan/grup/varlık)   (gerçek kayıt)    (hedef başına)
                       |                                   |
           tek sahibi backend/Entity Registry      yetkili dağıtım planı
                                                           |
                       +-----------------------------------+
                       |                 |                 |
                  USB doğrudan      BLE doğrudan      Gateway / LoRa
                       \                 |                 /
                        +-------- HEDEF TRACKER ----------+
                                     |
                            GeofenceMutationOwner
                         GeofenceStore + runtime + RESULT
```

- **Paylaşılan uygulama durumu:** Backend saklar ve yetkiye göre cihazlara sunar. Telefonun yerel deposu kanonik değildir.
- **Gözlem defteri:** Tracker kaydı güvenli/kimlikli alınır, Edge dayanıklı taşır, backend doğrular. Telefon B bu veri kaynağından yetkili konum/olayları indirir. Aynı kaydı her telefonun yeniden Gateway'den alması zorunlu değildir.
- **İstenen cihaz durumu ve gerçek etkin durum:** Ayrıdır. Backend veya telefon bir geofence için 'uygulandı' iddiasında bulunamaz; yalnız ilgili Tracker'ın doğrulanmış RESULT/reconciliation kanıtı kesinleştirir.
- **Edge (Android veya PC):** Veri toplar, çevrimdışı saklar, sıraya alır ve iletir; Gateway, backend veya sahiplik otoritesinin yerine geçmez.
- **Relay:** Yalnız onaylı paketleri taşır/aktarır; Gateway ya da tasma ayar otoritesi değildir.

## 3. Hangi veriler senkronize olur?

| Kaynak | Yetkili ekranlar arasında ortak mı? | Kanonik sahip / not |
| --- | --- | --- |
| Mera / alan çokgenleri, adları ve silme geçmişi | Evet | Backend, revizyonlu |
| Hayvan kimlikleri, küpe, not, fotoğraf | Evet, izinli | Entity Registry; erişim politikası |
| Hayvan-grup üyeliği ve alan hedef seçimi | Evet | Backend; hayvan/tasma bağlamı ayrı |
| Cihaz-hayvan atama geçmişi | Evet | Geçerlilik zamanı + kayıt zamanı; geçmişi silme |
| Konum, aktivite, pil, geçmiş ve gerçek EVENT | Evet, izinli | Kimlik doğrulanmış gözlemler; ölçüm ve geliş zamanı ayrı |
| İstenen rapor aralığı, istenen alan sürümü ve işlem kuyruğu | Evet, yetki dahilinde | Backend desired state / command service |
| Tasma başına teslim, ret, gerçek uygulama RESULT'u | Evet | Kanıt kaynağı ve zamanını göster |
| Paylaşılan yer işaretleri, ağ/istasyon adları, açıklama ve notlar | Evet | Backend, uygun izinle |
| Her kullanıcının gördüm/okundu işareti | Duruma göre kullanıcıya özel, sunucuda | Başka kişinin alarmını otomatik kapatmaz |
| Tema, harita kamera konumu, ekran düzeni | Hayır, varsayılan yerel | İstenirse ileride kullanıcı tercihi eşitleme |
| İndirilen çevrimdışı harita dosyaları, Bluetooth bağlantısı, USB portu | Hayır | Her cihazın fiziksel kaynağı ve kapasitesi ayrı |
| Cihaz root anahtarı, kişisel erişim sırları, debug kayıtları | Asla genel ortak veri değil | Ayrı güvenlik yetkisi / korumalı depolama |

Çizim dosyası **GeoJSON içe/dışa aktarmada kalır**. Cihazlar arası normal
eşitlemenin kullanıcıdan dosya indirmesini istemesine **gerek yoktur**.

## 4. Kalıcı sunucu veri modeli (uygulama sözleşmesi)

Önerilen mantıksal ayrımlar (fiziksel SQL tabloları/URL'ler henüz dondurulmadı):

- `site_id` / `owner_id` / `actor_id` / `installation_id`: kullanıcının hangi sahaya, hangi tasmalara ve hangi işleme erişebildiği;
- `zone_id`: sabit kimlik; `name`, **E7 kanonik köşeler**, `enabled_in_draft`, `revision`, `updated_by`, `server_committed_at`, `deleted_at/tombstone`;
- `group_id`: ad ve revizyonlu üye ilişkileri; kaynak kimlikleri hayvan `entity_id` ile tasma `device_id` arasındaki atanma geçmişini korur;
- `target_policy_id`: site/cihaz/grup/alan hedefleri; istemci ekranında görülen `desired_revision` ve işlem şablonu; bir grup değişince önceki uygulama sonuçları otomatik 'uygulandı' olmaz;
- `sync_op`: `op_id` (idempotency), `actor_id`, `installation_id`, `resource_id`, `base_revision`, `operation`, `payload`, `server_sequence`, `result`;
- `command_operation`: **ayrı** `operation_id`, kullanıcı yetkisi, hedef tasmalar için çözülmüş anlık liste ve her hedefin kanonik geofence digest/revizyonu, `requested_at` ve geçerlilik;
- `command_target_result`: `device_id`, denenen aktarım yolu, kalıcı kuyruk, autentik cihaz sonucu, belirsizlik ve son uzlaşma kaynağı.

Backend her kabul edilen paylaşılan değişikliğe **sunucu işlem sırası** verir.
İstemci `cursor` ile değişiklikleri idempotent çeker; silmeler tombstone olarak
taşınır. Offline sıradaki işlemler yeniden gönderilebilir, aynı `op_id` bir kez
uygulanır. Telefon saati ne yetki ne de kesin revizyon kaynağıdır.

**Aday API biçimi, uygulandı iddiası değildir:**

- `GET /api/v1/sync?cursor=...`: yetkiye göre değişiklikler + yeni cursor;
- `POST /api/v1/sync/ops`: idempotent toplu değişiklik, her op için ACCEPTED / CONFLICT / REJECTED / RETRYABLE;
- `POST /api/v1/geofence-operations`: hedefler çözüldükten ve yetki doğrulandıktan sonra **ayrı** uygulama talimatı;
- `GET /api/v1/geofence-operations/{id}`: hedef bazında teslim/uygulama sonucu;
- değişim haberi için SSE/WebSocket/polling sonradan seçilebilir; **push haber verme, kabul/onay kanıtı değildir**.

İlk uygulama çekirdeği bir site ve az sayıda cihazla doğrulanabilir; ancak
site/actor erişim sınırını daha sonra eklemek yerine baştan koymak gerekir.

## 5. Eşitleme ve çakışma kuralları

- **Taslak kaydetme** yerel depoyu önce kalıcılaştırır; internetsiz kaydetme
  mümkündür. Sunucuda kesinleşme ayrı etiketlenir. Başka cihazda görünme ancak
  başarılı eşitleme ve yeniden çekmeden sonra mümkündür.
- Bir taslağın `base_revision` değeri eskiyse sunucu **CONFLICT** döndürür,
  mevcut otoriteyi değiştirmez. İstemci iki sınırı karşılaştırıp yeni revizyon
  olarak tekrar kaydetmeyi önerir; harita çokgenlerini sessizce birleştimez.
- Farklı kaynakların revizyonları birbirini bloklamak zorunda değildir;
  tehlikeli bağımlılıklar (geometri ile hedef seçimleri) sunucuda bütünlük
  kontrolünden geçirilir.
- Silme, eski telefonu tekrar bağladığında alanı diriltmeyecek tombstone
  üretir. İzinler, audit ve gerekli geçmiş için tombstone saklama politikası
  ayrı belirlenir.
- Mevcut `orun.app.zones.v1` ve `orun.app.zoneTargets.v1` **silinmez**.
  Geçiş önce okuma/validasyon/yedek, sonra stable ID ile idempotent import ve
  sunucudan kabul kanıtı, ardından aktif senkronize depo kullanımıdır.
- Eşitleme hatasında son sağlıklı yerel görüntü korunur; kimlik/yetki
  reddinde aynı kaydı başka hesaba taşımaya çalışılmaz.
- Offline bir istemcinin yetkisi iptal edilmişse yetki güncellemesi sonrası
  sunucu yeni mutasyonları reddeder. Çevrimdışı etkili yetkilerin süresi
  ayrı güvenlik tasarımıyla sınırlandırılır.

## 6. USB, BLE, LoRa — aynı kaynak, farklı ulaşım

### USB — doğrudan Tracker'a

Firmware'de yerel `APP FENCE BEGIN/V/AREA/COMMIT` ve CLEAR yolu var.
PR #192 bilgisayardan *explicit* USB hazırlama aracı ekledi; Android doğrudan
USB-OTG yazıcısı, masaüstü UI, tam geometri readback/digest ve kimlik bağlama
henüz yok. USB'nin fiziksel erişim güven modeli, uzaktan yazma yetkisi sayılamaz.
Bir PC Edge'in Gateway seri portuna yazmak **Tracker'a uygulama değildir**.

### BLE — yakın Tracker'a

Android/PC, yetkili doğrudan BLE oturumunda aynı tam kaynak adayı parçalara
ayırıp tasma mutation owner'ına iletir. BLE eşleşmesi tek başına uygulama
yetkisi değildir. Sınır gönderimi, yetkilendirme, aktarım resume, CAS ve imzalı
sonuç/doğrulanmış durum çalışmadan 'uygulandı' görünmez. Bugünkü BLE uygulama
status/Edge veri akışı geofence mutation yazıcısı değildir.

### LoRa — uzaktaki Tracker'a

Telefon/web/PC -> Backend veya yetkili çevrimdışı Gateway/Edge -> **Gateway LoRa
downlink** -> gerektiğinde Relay -> hedef Tracker. Telefonun kendi BLE bağlantısı
LoRa donanımı yerine geçmez; telefon tek başına LoRa RF göndermez.
Gateway yalnız yetkilendirilmiş aktarım/opsiyonel ayrı delege yetki
çerçevesiyle çalışır. Relay Gateway gibi 'uygulandı' sonucu üretemez.

Tam alanlar tek LoRa pakete sığmayabilir. Kaynağın digest/precondition sorgusu,
parça kimliği, aralık/duty-cycle bütçesi, hata halinde devam, uyuyan tasmanın
alım penceresi, korumalı yeniden gönderim ve **sahte ACK direnci** gerekiyor.
En kritik konum/alarm paketleri toplu alan dağıtımınca engellenmez.

**REPLACE_SET** tam kaynak uygulama olmalı; **CLEAR_SET** yalnız ayrı onaylı
mutasyonla yapılmalı. Hiçbir zaman alanın telefonda kapatılması/silinmesi,
paket kaybı veya boş GeoJSON kendiliğinden cihazı CLEAR etmez.

## 7. Uygulama sonucu — her ekranda aynı doğruluk

```text
Taslak (yerel) -> Sunucuda senkronize
                 (bu, tasmaya yüklendi demek değil)

Kullanıcının 'Tasmaya uygula' eylemi ->
  Hedefe özel istek -> Yetkili kuyruk -> Gateway/yerel kanal ->
  Teslim kanıtı -> Tracker tam kaynak COMMIT ve doğrulanmış RESULT ->
  UYGULANDI
```

Her hedef için ayrı durum: `PENDING`, `DELIVERED_TO_GATEWAY`,
`DELIVERED_TO_DEVICE`, `APPLIED`, `ALREADY_SATISFIED`,
`STALE_PRECONDITION`, `REJECTED`, `UNCONFIRMED`,
`STATE_UNCERTAIN`, `OFFLINE`. `TX_DONE`, Gateway custody ACK, BLE write
veya sunucunun isteği kabulü `APPLIED` değildir. Bir grup içindeki 10
tasmadan 8'i uyguladıysa UI **8/10 uygulandı, 2 bekliyor/belirsiz** der.

Gerçek dışarı alarmı güvenli Tracker EVENT kaydından gelir; yalnız telefonda
koordinat-poligon karşılaştırması güvenlik alarmı yapmaz.

## 8. Güvenlik ön koşulları

- Kullanıcı, saha ve tasma bazında yetki. Örneğin owner / manager / viewer;
  `view_zones`, `edit_zones`, `apply_fence` ayrı izinler olabilir.
- Her Android kurulum/PC istemcisinin ayrı kimliği ve iptal edilebilir
  oturumu olmalı (Step 3C). **Bench ortak bearer token / Tailscale ağı erişimi
  tek başına çok kullanıcı yetkisi değildir.**
- Backend veriyi `site_id`/sahiplik ile ayırır; her okuma/yazma/komut
  bu filtreyi atlayamaz. Sırların, Tracker `K_root`'unun ve kimliklerin
  genel shared-zone API üzerinden paylaşılması yasak.
- Yalnız yetkili cihaz sahibi/yönetici tasmaya komut çıkarabilir. İmzalı
  veya korunmuş uygulama RESULT doğrulanmadan etkin durumu yayınlanamaz.
- Yerel fiziksel erişim USB kabulü ve internet üzerinden uzak komut güvenliği
  ayrı tehdit modelleridir. Kontrolsüz firmware yazma/erişim açılmayacak.

## 9. Gerçek geliştirme sırası

| Dilim | Yapılacak | Tamamlanma kanıtı |
| --- | --- | --- |
| S1: Kimlik/Yetki | Step 3C hesabın site/cihaz erişimi, her telefonun ayrı kimliği, yetkili backend oturumu | Telefon B izinliyken erişir, izinsizken 403; kayıp telefon iptali |
| S2: SharedStore | Backend SQLite/PostgreSQL seçimi, revizyonlu Zone/Group/Asset/Assignment, op journal, tombstone, cursor, ACL | Çift istemci aynı veri, idempotent tekrar, çatışma ve DB restart testleri |
| S3: Offline Android | Mevcut yerel taslakları silmeden taşı, durable outbox, pull/push, sync durumları | A çevrimdışı çizer, B çevrimiçi görür; A yeniden bağlanınca converge; iki offline edit çakışır |
| S4: Web/PC | Aynı API ve ACL ile paylaşılan alan/varlık ekranları; PC Edge veri toplama rolü bozulmaz | Web ve telefon aynı `zone_id`, geometri, revizyon |
| S5: Yerel uygulama | Tracker USB/OTG ve yetkili BLE resource transfer, tam kaynak/result kanıtı | Önceki sınır bozulmadan parça yarıda kalır, ancak tam/valid COMMIT ile değişir |
| S6: Uzak uygulama | Backend → enrolled Gateway → LoRa/Relay → Tracker, parçalı aktarım, RF/duty-cycle ve durable RESULT | Geç ACK/cihaz uykusu/kanal kesintisi altında yanlış APPLIED yok |
| S7: Genel senkronizasyon | Notlar, olaylar, ortak ayarlar, erişilebilir diğer ürün varlıkları aynı altyapıya alınır | Bir telefonda değişiklik diğer yetkili ekranlarda; her türde açık sahiplik/çatışma |

**S1/S2/S3 önceliklidir:** Kullanıcının gördüğü sınırı bütün ekranlara
aktarmak için LoRa geofence mutation'ın bitmesini beklemeye gerek yoktur.
Buna karşılık **güvensiz ortak token ile paylaşılan hesap/ACL varmış gibi
gösterilmeyecek**.

## 10. İlk kabul senaryosu

- A ve B aynı yetkili sahada; A alan `z1` çizer, sunucu kabul eder, B ve web
  aynı E7 köşeleri/revizyonu görür.
- C başka saha hesabında; `z1` ya da hedef tasmayı sorgulayamaz.
- A offline alan `z2` çizer; A'da 'Bu cihazda' görünür, B'de henüz yoktur.
  Bağlantı döndüğünde kabul edilmiş revizyon diğer uçlara yayılır.
- A ve B aynı alanı offline değiştirir: B'nin eski revizyonu 409/CONFLICT;
  ikisi de veri kaybetmez, hiçbir tasma kendiliğinden yeni alana geçmez.
- A bölgeyi seçip 3 tasma için uygular: 1 USB APPLIED, 1 BLE APPLIED,
  1 LoRa UNCONFIRMED. B ve web aynı **1/1/1 ayrımını** görür.
- Önceki GeofenceStore etkin seti, kesilmiş BLE/LoRa aktarımında korunur.
- Telefonun silinmesi/bozulması daha önce sunucuya kabul edilmiş alanı silmez.
- Ayrı iki Edge aynı sealed gözlemi backend'e yollayınca tek yetkili gözlem
  olur; ortak UI'da iki ayrı konum oluşmaz.
