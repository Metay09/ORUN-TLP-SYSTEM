# ORUN — Backend sürekli açık (Debian 12, özel Tailscale ağı)

27.10.2026 · Dağıtıma hazır sayılmayan **bench altyapısının** kalıcı
çalışma kolaylığı. Gateway/tasma firmware'i, kripto protokolü ve
mevcut backend veritabanı dosyasının yolu değiştirilmez.

## Neden Edge → Backend aktarılmayabilir?

- Backend şu anda yalnız
  `python3 backend/orun_backend_bench.py --host ... --port 8090 --token ...`
  komutu çalıştığı sürece hizmet veriyor; repo içinde varsayılan kurulu
  servis tanımı yok. Önce yerel PC'de süreç dinliyor mu doğrula.
- ORUN Edge telefonda `Ağ → Sunucu` adresi, token ve
  `Kendiliğinden gönder` ayarı var. Otomatik açık ve URL/token doluysa
  uygulama **30 saniyede bir**, yeni kayıt sonrası da **~5 saniye** içinde
  yüklemeyi dener. Sunucu kapalıyken kayıtlar telefondaki ledger'da
  kalır; telefon backend sunucusunu açmaz.
- Sağlık kontrolü `GET /v1/health` (HTTP 200,
  `{"ok":true,"records":N}`); başarılı olması token yetkisini kanıtlamaz.
  Doğru token için `GET /v1/edge/keys` yetkili olmalı.
- Uzak backend için Tailscale **backend PC** üzerinde çalışmalı.
  Edge'in yazdığı URL, backend PC'nin Tailscale IPv4 adresi
  `http://<backend-tailnet-ip>:8090`. Port 8910, APK dosya sunucusudur,
  ORUN backend değildir.

## Kalıcı systemd kullanıcı hizmetini güvenle kur

**Yalnız gerçekten backend veritabanı ve gerçek credential registry dosyalarının
bulunduğu Debian bilgisayarında çalıştır.** Gateway'in takılı olduğu
bilgisayarın backend ile aynı makine olması GEREKMEZ. Yanlış PC'ye
yeni boş credential registry oluşturulmasın diye installer
`~/orun-credentials.sqlite` dosyası yoksa çalışmayı reddeder.

Bu depo/branch'te:

```sh
bash tools/ops/orun_backend_service.sh install
```

Kurulum ekranda **telefon Edge uygulamasına daha önce girilmiş mevcut
token'ı** gizli olarak bir kez ister. Token değişmez; shell history,
ps komut satırı veya GitHub'a yazılmaz.
`~/.config/orun/backend.token` dosyasında `0600` saklanır.
Varsayılan DB: `~/orun-backend-bench.sqlite`.
Varsayılan gerçek credential registry: `~/orun-credentials.sqlite`.
Bu iki dosya silinmez, taşınmaz, sıfırlanmaz. Farklı yerde mevcut kayıtlar
varsa **kurulum yapmadan önce doğru konum doğrulanmalıdır**.

Kurulum oluşturur:
- `~/.config/systemd/user/orun-backend.service`
- `systemctl --user enable --now orun-backend.service`
- `Restart=always`, `RestartSec=10`
- `tailscale ip -4` ile yalnızca **backend bilgisayarının Tailscale
  IPv4 arayüzüne** bağlanan sunucu (`0.0.0.0` kullanılmaz).

Bilgisayar yeniden başladığında kullanıcı oturumu açılmadan da
başlamasını sağlamak için **bir defalık**:

```sh
sudo loginctl enable-linger "$(id -un)"
```

Hizmet durumunu ve HTTP sağlığını kontrol et:

```sh
bash tools/ops/orun_backend_service.sh status
bash tools/ops/orun_backend_service.sh logs
```

`/v1/health` 200 ve `systemctl --user is-active` çıktısı `active`
olmalı. Servis `failed` ise 8090 başka süreçte olabilir; yeni sunucu
açmadan önce çalışan servisi, logları ve kullandığı **aynı** DB/registry
dosyalarını denetle. Rastgele başka token ya da veritabanı oluşturma.

## Android tarafı

- ORUN Edge → **Ağ → Sunucu**: `http://<backend-PC-Tailscale-IP>:8090`.
- **Anahtar:** hizmet kurulumunda girilen mevcut ORUN Edge token'ı.
- **Kendiliğinden gönder:** açık; **Kaydet**.
- **Şimdi gönder** ile test et.
- Başarılı kayıt sayısı ve bekleyen kayıt sayısı gözlemlenmeli. `401`
  token eşleşmiyor; zaman aşımı backend/Tailscale/port; `REJECTED`
  doğru registry/epoch anahtarı olmadan çözülemez.
- Uygulama servis monitörü değildir: telefondaki ayar backend daemon'un
  aç/kapat düğmesi DEĞİLDİR.

## PC Edge ayrı iş akışı

Bu değişiklik backend'in **7/24 bir process olarak çalışmasını** sağlar;
Android telefon olmadan PC'nin Gateway'den okuyup uzak backend'e
yüklemesini **tek başına sağlamaz**. PC Edge için sürekli çalışan ayrı
USB/BLE collector + dayanıklı yerel ledger + backend upload worker
gerekir. `tools/orun_edge_bench.py` şu anda bunun bench alıcısıdır,
tüm-anahtar / yetkili sürekli PC Edge servisi değildir.

## Güvenlik ve sürüm kapıları

Bu `orun_backend_bench.py` **henüz üretim-grade backend değil**:
token aynı Tailscale tailnet içindeki tek-owner yetkisi, HTTP yalnız
Tailscale içindedir; `GET /api/points` bench haritası Tailscale
erişimi olan istemcilerce okunabilir. Public internet'e açma.
`--bench-test-key` özelliği varsayılan service'e **eklenmez**.
Real credentials `~/orun-credentials.sqlite` üzerinden okunur.
Veri tabanı için normal SQLite dayanıklılık kuralları kullanılır;
backup/operasyonel izleme ve gerçek üretim sertleştirmesi ayrı iş.

Test gate (yeni branch):

```sh
bash -n tools/ops/orun_backend_service.sh
python3 firmware/tests/sf5/test_orun_backend_service.py
python3 firmware/tests/sf5/test_sf5l8_backend_sync.py
```

Başarılı test, Debian'da sürekli çalışma veya gerçek Gateway aktarımının
fiziksel doğrulaması yerine geçmez. Hizmeti kişisel bilgisayara henüz
hiçbir araç uzaktan kurmadı.
