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
# 1) Önce host testleri. Gateway'e hiçbir komut gönderilmez:
bash -n tools/ops/orun_pc_edge_service.sh
python3 firmware/tests/sf5/test_orun_pc_edge.py

# 2) Gerçek Gateway ID, USB kart seri numarasıyla aynı varsayılmamalıdır.
#    Bu değer PC Edge veritabanındaki HELD kayıtlarından okunur:
GATEWAY_ID="$(python3 - <<'PY'
import sqlite3
from pathlib import Path
db = Path.home() / ".local/share/orun/pc-edge-inbox.sqlite"
with sqlite3.connect(db.resolve().as_uri() + "?mode=ro", uri=True) as c:
    rows = c.execute("SELECT DISTINCT gateway FROM sealed_records "
                     "WHERE gateway IS NOT NULL").fetchall()
if len(rows) != 1:
    raise SystemExit(f"Tek bir Gateway bekleniyor, bulunan: {len(rows)}")
print(rows[0][0])
PY
)"

# 3) Mevcut USB servisini SF5L0 BENCH moduna geçirir
#    (servis yeniden başlar; veri dosyaları silinmez):
bash tools/ops/orun_pc_edge_service.sh bench-release enable "$GATEWAY_ID"

# 3) Beklenen gözlem: [bench accept] sent / gateway result: OK.
bash tools/ops/orun_pc_edge_service.sh logs

# İstendiği an geri alma. Default receive-only servisine döner:
bash tools/ops/orun_pc_edge_service.sh bench-release disable
```

Bu komut yalnızca `ORUN_SF5L0_BENCH_TEST_KEYS` derlemesi olan USB Gateway ile uyumludur. Gateway'in normal üretim firmware'i aynı PUBLIC TEST KEY teslim onayını **tanımaz**. Sunucunun STORED/DUPLICATE cevabı görülmeden, reddedilen ya da belirsiz kayıtlar için hiçbir ACK gönderilmez. Bench onayı bizzat Gateway tarafından kabul edilmediği sürece kaydın serbest bırakıldığını söylemeyin.

## BLE teslim onayı (host test aşaması)

USB'de ölçülen `held=0/1056`, `EDGE DUMP END count=0` ve `reclaimed=59` 28 Ekim 2026 itibarıyla **SF5L0 USB Gateway** sahasında teyit edilmiştir. Bunlar BLE test sonucu değildir.

PC Edge BLE alıcısı artık Gateway HELLO'dan kimliği alır; HELD/PREVIEW içindeki `page/slot/generation` ile şifreli kaydı eşleştirir. Backend STORED/DUPLICATE onayı SQLite'a kaydedilmeden SF5L0 ACCEPT göndermez. BLE için `edge_accept_binary` ve `edge_frames` aynı Android/host protokolünü kullanır. Gateway'in 0x04 ACCEPT_RESULT bildirimi, işlemdeki kaydın page/slot eşleşmesiyle işlenir; BUSY yeniden denenir, reddedilen gönderimler serbest bırakılmaz. `--bench-release-gateway` verilmedikçe BLE de alıcı-only kalır.

```sh
# Canlı sisteme dokunmayan BLE+USB regresyon testi:
python3 -m py_compile tools/orun_pc_edge.py
python3 firmware/tests/sf5/test_orun_pc_edge.py

# PC'de BLE çalıştırmak için sürücü kontrolü:
python3 -c 'import bleak; print("BLEAK: HAZIR")'
command -v bluetoothctl
```

**Aynı Gateway için USB servisi açıkken ikinci bağımsız PC Edge BLE servisi başlatmayın.** Önce bluetooth aygıtı ve testler doğrulanmalı, ardından transport kontrollü şekilde değiştirilmelidir. Test anahtarıyla yapılan SF5L0 handoff gerçek üretim kimlik doğrulaması değildir.

## Gerçek BLE bağlantı doğrulaması ve USB/BLE geçişi

28.10.2026 gerçek BLE saha testi: cihaz \`ORUN-4B275BA5\`, BLE adresi \`C5:DD:01:85:C9:4B\`. Bleak GATT connection \`True\`, SF5L6 HELLO kimliği \`09A462BD4B275BA5\`, \`held=0/1056\`, \`DUMP END count=0\`. **BLE bağlantı/okuma PASS**, BLE'de yeni kayıt alma + backend yükleme + HMAC ACCEPT henüz canlı kayıtla sınanmadı.

Kurulu user-service zaten USB + bench-release modundaysa, aşağıdaki komut aynı \`orun-pc-edge.service\` dosyasına öncelikli \`20-ble-transport.conf\` drop-in ekler. Böylece **aynı anda iki PC Edge uploader çalışmaz**. Backend URL, token, Edge ID ve test Gateway kimliği olduğu gibi korunur. USB cihazı sökmek gerekmez.

\`\`\`sh
# Geçiş doğrulama ve host regresyon (hizmetleri değiştirmez)
bash -n tools/ops/orun_pc_edge_service.sh
python3 -m py_compile tools/ops/orun_pc_edge_transport.py tools/orun_pc_edge.py
python3 firmware/tests/sf5/test_orun_pc_edge.py
bash tools/ops/orun_pc_edge_service.sh transport validate ble C5:DD:01:85:C9:4B

# SADECE ONE SERVICE: USB'den BLE'ye geç (PC Edge servisi yeniden başlar)
bash tools/ops/orun_pc_edge_service.sh transport ble C5:DD:01:85:C9:4B
journalctl --user -u orun-pc-edge.service --since "2 minutes ago" --no-pager -o cat |
  grep -E '^\[ble\]|^\[backend\]|^\[bench accept\]' | tail -n 60

# Geri dönüş: BLE drop-in kaldırılır ve önceki USB bench-release çalışır
bash tools/ops/orun_pc_edge_service.sh transport usb
\`\`\`

USB bağlantı ve depolama geri kazanımı doğrulanmıştır. BLE modunda önce \`[ble] HELLO ...\` ve \`[ble] DUMP END ...\` arayın. **Yeni Tracker kaydı yoksa BLE üzerinden backend STORED ve ACK başarısı iddia edilemez.** Yeni kayıt geldikten sonra \`[ble] HELD/PREVIEW stored\`, \`[backend] confirmed\`, \`[bench accept] gateway result: OK\` beklenir. Gateway'in \`0x05\` RECLAIM durumu da loglanır.

BLE modunda devredeki anahtar SF5L0 **PUBLIC TEST KEY**'dir; gerçek üretim yetki protokolü ayrıca ele alınmalıdır.

## Saha doğrulama kapısı

1. Backend gerçek bilgisayarında `orun-backend.service` **active**, `GET /v1/health` 200; doğru tokenla `/v1/edge/keys` 200; gerçek DB kayıt sayısı korunmuş.
2. PC Edge Gateway'e USB/BLE ile bağlanmış; `sealed record committed` görülüyor; `pending` sayısı artıyor.
3. Backend bağlantısı varken `confirmed N` logu görülüyor; `pending` azalıyor; backend kayıt sayısı yeni autentik kayıtlarla artıyor.
4. Backend ağ bağlantısı kesilirken PC Edge spool tutuyor; bağlantı dönünce aynı kayıtlar tekrar yükleniyor, mükerrer/backend çakışma denetimi korunuyor.
5. Uzun süreli Gateway custody dolum testi ve backend doğrulamalı imzalı release ayrı iş paketi. Bunlar geçmeden tüm sistem 'bitmiş/üretime hazır' sayılmaz.

**Durum:** Kaynak kodu ve host testleri hazırlanmıştır. PR taslaktır. Gerçek Debian servisi, cihaz bağlantısı ve uçtan uca transfer bu belgeyle kendiliğinden kurulmuş değildir.
