# ORUN S6A — Hesap girişi ve hesap kapsamlı ortak veri erişimi

**Durum: GITHUB DRAFT / HOST TEST PENDING. Canlı sunucu, çalışan 4
systemd servisi, Tailscale 443/8443, Tracker ve APK değiştirilmedi.**

Amaç: Yeni telefona ORUN kurulduğunda kullanıcı adı ve şifresi ile giriş
yapabilmek. Bu ilk parça gerçek **hesap kimlik doğrulaması ve site-kapsamlı
ortak mera/grup/hedef taslaklarına erişim** getirir. Henüz hayvan kayıtları,
konum geçmişi ve otomatik Android eşitlemesi yok. Genel ürün "hazır" değildir.

## Güvenlik ve iş akışı

1. Telefon önce S5A/S5B/S5C kapsamında **P-256 Android Keystore özel
   anahtarını** oluşturur, sahibin tek seferlik onayıyla kayıt olur.
   Sadece telefon kaydı, ortak mera verilerinin okunmasına/yazılmasına
   yetmez. PC Edge'in eski Ed25519 read-only bearer'ı korunur.
2. İlk hesap yalnız sahibin fiziksel olarak eriştiği makinede,
   `tools/orun_account_bootstrap.py` ile açık onay ve iki kez
   şifre girişi üzerinden oluşturulur. CLI argümanı veya varsayılan
   şifre yok. Hesap site + aktör ile bağlıdır.
   16 byte rastgele salt, stdlib scrypt N=32768/r=8/p=1,
   salt+verifier yalnız özel izinli SQLite içinde tutulur.
   Parolayı telefon yerel depoya veya uygulama günlüklerine yazmayın.
3. Sunucuda yalnız açık `--enable-account-login` seçeneğiyle
   `POST /api/v1/account/login` hizmeti sunulur. İstek
   `Authorization: Bearer <önceden onaylı telefon tokenı>` ve yalnız
   `{username,password}` alanlarını kabul eder.
   `site`, `actor`, `scopes`, `role` istemci tarafından
   seçilemez. Aynı site+aktör hesabı eşleşmiyorsa login reddedilir.
   Yanlış parolada **hesap veya telefon varlığı ifşa edilmez**.
   5 başarısız girişten sonra telefon kurulumunda 15 dakika
   geçerli kalıcı kilit uygulanır.
4. Başarılı girişte 48-byte rastgele account bearer, **12 saat**
   ömürlü; veritabanında yalnız SHA-256 digest tutulur.
   `account_sessions` mevcut onaylı P256 telefonun
   `installation_id + device_token_hash` değeriyle bağlıdır.
   Telefon tokenı döner, sona erer veya iptal edilirse eski hesap
   bearer'ı da reddedilir. Girişten doğan `Actor`, mevcut
   telefonun izinleriyle hesap sahibinin önceden tanımlı ortak veri
   izinlerinin **kesişimidir**; tek başına tasma/LoRa komut yetkisi
   çıkarmaz.
5. `GET /api/v1/account/me` hesap kimliğini gösterir;
   `POST /api/v1/account/logout` account bearer'ı iptal eder.
   `GET /api/v1/sync` ve `POST /api/v1/sync/ops`,
   hesap modu açıkken P256 telefon için hesap bearer'ı gerektirir;
   salt telefon bearer'ıyla artık ortak veri erişimi yoktur.
   Telefonu onaydan sonra doğrulamak için eski
   `GET /api/v1/sync/identity` istisnası korunur.
   PC mirroring Ed25519 `pc-*` kurulumları hesap girişi
   gerektirmeksizin önceki read-only yetkileriyle çalışır.
6. Android `edge/app/src/app/ownerAccountClient.ts`: HTTPS-only,
   sadece açık kullanıcı eylemiyle login; hesap bearer'ını **yalnız
   bellekte tutar**; 12 saat içinde `/api/v1/sync?cursor=...` ile
   yetkili veri okuma için client adayını sağlar.
   Henüz kurulum ekranına, gerçek SQLite eşitlemeye veya
   nihai hesap arayüzüne bağlanmamıştır. Uygulama yeniden açıldığında
   tekrar giriş gerekecektir. Nihai ürün için korumalı account
   oturum depolama/yenileme politikası ayrıca yapılacaktır.

## Uygulama sınırı

`tools/orun_shared_sync_loopback.py` mevcut komutlarla tamamen aynı
çalışır. Hesap modu açıkça seçildiğinde yalnız daha önce
oluşturulmuş, 0600 izinli `owner_accounts.sqlite` kabul edilir;
aksi halde **fail closed**. Bu aşamada bu bayrağı **gerçek
`~/ORUN-SHARED-RUNTIME` üzerinde açmayın**: testler ve geçiş/
rollback planı bitmeden mevcut PC mirroring, telefon ve sunucu
erişim yolları değiştirilemez.

Aday `account_handler_factory` telefona kayıt endpointlerini
ayrıca `--enable-phone-enrollment` açılmadıkça sunmaz;
hesap açma/şifre sıfırlama/hesap yükseltme için public API YOK.

## Kabul kontrolü (henüz yerelde yürütülmedi)

```sh
bash tools/ci/run_owner_account_login_host.sh
```

Geçici SQLite dosyaları ve yalnız 127.0.0.1 HTTP.
Yeni host testleri:
- Hesap girişi olmadan mera/grup taslaklarına erişim 401
- Sahip hesabıyla giriş, hesap oturumu, sunucudan kimlik, yaz/oku
- Aynı sahip için **iki ayrı onaylı telefon** ortak mera kaydını görüyor
- Farklı site+aktörde veri izolasyonu
- Eski Ed25519 PC mirroring okuma izni ve Android login reddi
- Telefonun iptali ve token döndürülmesi hesap oturumunu kesiyor
- Başarısız parola giriş sınırı ve veritabanı yeniden açılınca korunması
- Gizli hesap bootstrap endpointi yok, anonim login yok
- Android TypeScript istemci hesabı HTTPS/telefon/site eşleştirme,
  kısa süreli bellek oturumu, logout, yetkisiz veri reddi
- S5A/B/C 34 mevcut Python testleri ve iki TS host testi regresyonu

**Test çıktısı PASS gözlemlenmeden tamamlandı yazılmamalıdır.**
Gerçek Android Java derlemesi daha önce 107 görev ile geçmiştir,
ancak bu dal Android UI/native cihaz testi değildir.

## Nihai ürüne kalan engeller

- Güvenli, sürdürülebilir kullanıcı hesabı yönetimi:
  parola değiştirme/kurtarma, çoklu kullanıcı, yardımcı roller,
  oturum yenileme, parola deneme saldırılarına karşı üretim
  rate limit/gateway denetimi, sunucu yedekleme.
- Hesap oturum bearer'ı hâlâ **bearer**; cihazda üretilen proof-of-
  possession veya eşdeğer bağlayıcı koruma olmadan yakalanan
  hesap tokenının farklı istemciden kullanım riski kalır.
  Bu parça üretim güvenlik onayı değildir.
- Android ekranında kullanıcı adı/şifre girişi ve cihaz onayı,
  gerçek native secret depolama/expiry/revoke UX.
- Sunucuda merkezî hayvan/collar/device registry, konum geçmişi
  ve izinlere göre owner hesabına bağlama.
- Android SQLite offline-first ledger, outbox/retry ve gerçek
  iki yönlü mera/grup/hedef eşitlemesi; `APPLIED` sonucu ancak
  Tracker tarafından doğrulanınca gösterilecek.
- Android OEM KeyStore, BLE/USB/LoRa, çoklu telefon, kesinti,
  gerçek saha ve roll-out/rollback testleri.
