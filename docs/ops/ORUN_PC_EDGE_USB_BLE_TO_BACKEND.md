# ORUN — PC Edge, telefonsuz USB/BLE → Backend (taslak)

27 Ekim 2026. Bu ilk PC Edge köprüsü **bench/saha testi** içindir; cihaz yazılımını değiştirmez.

## Veri yolu ve güvenlik sınırı

Gateway --(USB veya BLE)--> PC Edge --(yerel SQLite WAL/FULL)--> Tailscale --> Backend

- PC Edge cihazın şifreli PRODUCT_SECURE çerçevesini **açmaz**. Root/Tracker credential, telefondan veya backend'den PC'ye kopyalanmaz.
- Aynı PC üzerinde veya **farklı Tailscale PC'de** backend çalışabilir. Backend'in veri anahtarları yalnızca onun gerçek credential registry'sinde olur.
- İnternet/Backend kesilince PC Edge kendi SQLite kuyruğunda veriyi tutar ve yeniden dener. Sunucu STORED/DUPLICATE demeden yerel kuyruktan teslim edilmiş işaretlemez.
- REJECTED/CONFLICT dosyadan silinmez; yerel karantinada tutulur. UNCERTAIN tekrar denenir.
- Eski kayıtların açılması güncel pozisyonu geciktirmesin diye her yükleme turuna *en yeni* bekleyen kayıtlardan bir tanesi öne konur; gerisi FIFO'dur.
- Tailscale IPv4, port 8090, kullanıcı dosyasında 0600 token; HTTP redirect ve proxy devre dışı. **Public IP / port-forward yok.**
- **Varsayılan:** PC Edge Gateway'e HMAC EDGE ACCEPT veya custody release göndermez; Gateway custody kayıtları tutulur. **Yalnızca USB SF5L0 bench testlerinde** `--bench-release-gateway <16-hex-Gateway-ID>` seçeneğiyle, önce backend STORED/DUPLICATE doğrulanmış şifreli kaydın tam `page/slot/generation/frame` eşleşmesi üzerinden mevcut **PUBLIC TEST KEY** HMAC onayı gönderilebilir. Yanıtlar ve denemeler tutulur; reddedilen ve belirsiz kayıtlar onaylanmaz. Bu üretim yetkilendirmesi değildir; gerçek cihaz üzerinde kullanılmaz. Üretim için ayrı, yetkili backend receipt ve Gateway release protokolü gereklidir.
- Benzer şekilde USB'deki `BENCH GW DUMP` komutu ve BLE Edge linki **SF5L0 bench protokolüne** göredir. Gerçek provisioned Gateway firmware'inin aynı veri yolunu desteklediği cihazla test edilmeden varsayılmaz.

## Gereksinimler

- Gerçek backend PC'de PR #186'nın kalıcı servisinin *mevcut doğru DB ve registry* ile çalışıyor olması; sistemin kendi Tailscale IPv4 adresi: `tailscale ip -4`.
- Gateway USB için `pyserial`; BLE için Linux BlueZ ve `bleak`. Örneğin Debian USB: `sudo apt install python3-serial`.
- PC Edge bilgisayarında ayrı yerel SQLite (otomatik `~/.local/share/orun/pc-edge-inbox.sqlite`). Backend'in `~/orun-backend-bench.sqlite` dosyasını burada kullanmayın.
- Token, mevcut ORUN Edge uygulamasına tanımlanmış **aynı** token; terminalde gizli istenir. GitHub, komut satırı argümanı, paylaşılan sohbet veya ekrana yazılmaz.
- USB portu: tercihen kalıcı `/dev/serial/by-id/` yolu; `ls -l /dev/serial/by-id/` ile doğrulayın. Rastgele `/dev/ttyACM0` diğer karta ait olabilir.
- BLE/USB aynı Gateway'e fiziksel olarak erişmek için iki ayrı seçenektir; ikisi aynı anda zorunlu değil. Android ile eşzamanlı BLE bağlantısı donanıma bağlı olarak mümkün olmayabilir.

## Bağımsız PC Edge hizmetini kur

Komutlar PC Edge'in çalışacağı Debian bilgisayarında, ilgili GitHub branch'inde:

```sh
# USB — son argümanda sizin gerçek Gateway portunuz bulunmalı:
bash tools/ops/orun_pc_edge_service.sh install usb \
  http://100.x.x.x:8090 pc-edge1 /dev/serial/by-id/GATEWAY

# VEYA BLE — belirtilen BLE servis UUID'sini tarar:
bash tools/ops/orun_pc_edge_service.sh install ble \
  http://100.x.x.x:8090 pc-edge1

# Yeniden giriş yapmadan oturumdan sonra da ayağa kalksın:
sudo loginctl enable-linger "$(id -un)"

bash tools/ops/orun_pc_edge_service.sh status
bash tools/ops/orun_pc_edge_service.sh logs
```

`100.x.x.x` örnektir: **gerçek backend PC'nin** `tailscale ip -4` değeri girilmeli. Telefondaki APK dağıtımı portu `8910`, backend değildir. Bir bilgisayardan diğerine `8090` bağlantısı sağlanmalıdır. Servis backend'i açıp kapatmaz.

## Önce host/simülasyon testleri

```sh
bash -n tools/ops/orun_backend_service.sh
bash -n tools/ops/orun_pc_edge_service.sh
python3 firmware/tests/sf5/test_orun_backend_service.py
python3 firmware/tests/sf5/test_sf5l8_backend_sync.py
python3 firmware/tests/sf5/test_orun_pc_edge.py
```

PC Edge testinde gerçek tasma/Gateway anahtarı kullanılmaz. Yerel geçici SQLite ve **public test vector** kullanılır; sahadaki gerçek DB'lere dokunmaz.

## USB SF5L0 test Gateway: isteğe bağlı teslim onayı

Önce host testleri ve gerçek backend mutabakatı başarılı olmalı. **Mevcut kurulu sistemd servisi aynı kalarak eski, alıcı-only modda çalışır.** CLI üzerinde `--bench-release-gateway 09A462BD4B275BA5` parametresi yazılmadıkça Gateway'e ACCEPT gönderilmez. Bu anahtar herkese açıktır: cihazın production/provisioned olduğunu düşünüyorsanız kesinlikle etkinleştirmeyin. Bu modu açmak için çalışan PC Edge hizmetiyle seri portu eşzamanlı açmayın; servis yapılandırması ayrıca doğrulanmalı.

Program, yalnızca backend tarafından STORED veya DUPLICATE yanıtı kalıcı kaydedilmiş tam çerçeveler için `EDGE ACCEPT v1` oluşturur. `OK`, `BUSY`, `UNKNOWN`, `REJECTED` ve zaman aşımı ayrı izlenir; aynı custody handle üç denemeden sonra durur. Gateway kendi HMAC doğrulamasını uygular. Bu bir bench protokol uyumluluk denemesidir; imzalı backend yetkisi veya üretimde güvenli tasma teslimi yerine geçmez.

```sh
# Önce sadece izole host testi; Gateway'e hiçbir komut gönderilmez:
python3 firmware/tests/sf5/test_orun_pc_edge.py
```

## Saha doğrulama kapısı

1. Backend gerçek bilgisayarında `orun-backend.service` **active**, `GET /v1/health` 200; doğru tokenla `/v1/edge/keys` 200; gerçek DB kayıt sayısı korunmuş.
2. PC Edge Gateway'e USB/BLE ile bağlanmış; `sealed record committed` görülüyor; `pending` sayısı artıyor.
3. Backend bağlantısı varken `confirmed N` logu görülüyor; `pending` azalıyor; backend kayıt sayısı yeni autentik kayıtlarla artıyor.
4. Backend ağ bağlantısı kesilirken PC Edge spool tutuyor; bağlantı dönünce aynı kayıtlar tekrar yükleniyor, mükerrer/backend çakışma denetimi korunuyor.
5. Uzun süreli Gateway custody dolum testi ve backend doğrulamalı imzalı release ayrı iş paketi. Bunlar geçmeden tüm sistem 'bitmiş/üretime hazır' sayılmaz.

**Durum:** Kaynak kodu ve host testleri hazırlanmıştır. PR taslaktır. Gerçek Debian servisi, cihaz bağlantısı ve uçtan uca transfer bu belgeyle kendiliğinden kurulmuş değildir.
