# ORUN S4E — mevcut gözlem servislerine dokunmadan kararlı ortak veri yan sistemi

**Durum (2026-10-29): DRAFT / S4E kullanıcı host doğrulaması **2/2 PASS** (`968e5a8`, `ORUN_S4E_STAGING_HOST_PASS`). Kurulum ve canlı etkinleştirme YOK.**

Önceki kullanıcı kanıtı: S4D gerçek süreç `850c7fc` **PASS**:
3 örnek mera/grup/hedef kaydı authenticated HTTP ile yazıldı; PC Edge
aynasına 3/3 geldi (cursor 3). Backend ayrı süreç yeniden başlatıldı.
2 DELETE tombstone PC'de aynalandı (cursor 5); grup kaldı.
Canlı `orun-backend.service` PID 303602, `orun-pc-edge.service`
PID 299910 korunmuştu. **Bu geçici deney gerçek saha konuşlandırması değildi.**

## Neden farklı bir çalışma klasörü?

`~/ORUN-SHARED-VERIFY.SxcWay` geliştirme worktree'sinde sürekli
`git switch --detach FETCH_HEAD` yapılıyor. Bu yoldan sürekli çalışan
servis başlatmak, yazılımların habersiz değişmesine sebep olabilir.
Ayrıca çalışan gözlem servisi `~/ORUN-PC-EDGE-VERIFY` kopyasından,
Tailscale 8090 portunda görevine devam ediyor. `orun_pc_edge.py` canlı
kopyası GitHub'da yoktur; bunu körlemesine değiştirmiyoruz.

## S4E hazırlayıcı

`tools/ci/stage_shared_pc_runtime.py` açık
`--approve-local-pc` verilmeden **hiçbir şey oluşturmaz**.
Verilirse SADECE **önceden bulunmayan** yeni bir dizinde:

- 0700 dizinler içinde Backend ve PC client'ın **altı Python kaynak
  dosyasının ayrı kopyasını** oluşturur; her dosyanın SHA-256 özeti ve
  Git HEAD manifestte yazılır. Gelecekte Git worktree değiştirilse de
  sistem kodu kendiliğinden değişmez.
- İki yeni `shared_resources.sqlite` ve `enrolled_edges.sqlite` ile
  ayrı PC salt-okunur `shared.sqlite` oluşturur; hepsi owner-only'dir.
  Mevcut PC Edge observation ledger ve 8090 Backend DB'ye dokunmaz.
- Sahip onaylı `pc-*` Step3C salt-okunur kurulum token'ını ayrı
  0600 dosyaya kaydeder, terminal ve manifestte bearer göstermez.
  **Token ömrü 30 gün**, otomatik yenileme yok; üretim kalıcılığına
  hazır güvenlik modeli sayılmaz.
- Yalnız `127.0.0.1:18781` (ya da boş başka özel port) için üç
  **kurulmamış** systemd user unit taslağı yazar:
  `orun-shared-sync.service` (127.0.0.1 loopback),
  `orun-shared-pc-mirror.service` (salt-okunur 0600 SQLite),
  `orun-shared-pc-mirror.timer` (bitimden 180 saniye sonra tekrar).
  Mirror, yalnız yeni shared Backend'i `Requires` ile başlatır;
  **asla** mevcut `orun-backend.service`'i başlatıp durdurmaz.
- `systemctl`, `daemon-reload`, `enable` veya `start` **çağırmaz**.
  `~/.config/systemd/user` içine hiçbir dosya yazılmaz;
  elle kurulmadan hiçbir yeni hizmet otomatik çalışmaya başlamaz.
  Kurulum dizini var ise **hiçbir şeyin üzerine yazmaz**.

## Host kabul testi (canlı duruma dokunmaz)

```bash
bash tools/ci/run_shared_pc_runtime_staging_host.sh
```

Yalnız `TemporaryDirectory` altında S4E hazırlığı ve yeni
`127.0.0.1` süreç başlatma sınanır:
1. Onay yokken dosya oluşmaması, var olan hedef dizin reddi.
2. Altı kod dosyasının SHA-256 ve 0600 izinleri.
3. Kopyadan bağımsız S4C Backend gerçek süreç başlaması,
   korumalı S4D PC tokenı ile S4A mirror GET'e `HTTP 200`
   ve `received=0` sonucu.
4. Staged unit içinde token olmaması, var olan 8090 portuna dokunulmaması,
   Backend'e bağımlılık, owner-only state/manifest.

**Kullanıcı doğrulaması: 2/2 PASS (0.615s), gerçek 127.0.0.1 HTTP, yalnız geçici SQLite; hiçbir kalıcı servis kurulumuna geçilmedi.**

## İlk kullanıcı testi ve düzeltme (2026-10-29)

- Kullanıcı `cd7ae0c` üzerinde S4E testini başlattı: **1 ImportError** ile test yükleme aşamasında durdu; testler çalışmadı.
- Hata: `backend/tests/test_shared_pc_runtime_staging.py` yanlışlıkla `tools.ci.render_shared_pc_timer.SERVICE` sabitini çağırıyordu. Bu modülde mevcut sabit `UNIT`.
- Sadece import adı `SERVICE as MIRROR_SERVICE` → `UNIT as MIRROR_SERVICE` olacak şekilde düzeltildi: `21bdebe`.
- **Düzeltilmiş kod 968e5a8'de kullanıcı tarafından çalıştırıldı: 2/2 PASS, `ORUN_S4E_STAGING_HOST_PASS`.** Hiçbir çalışan ORUN servisi değiştirilmedi.

## Kalıcı dizin hazırlığı tamamlandı (29 Ekim 2026)

Kullanıcının gerçek terminal çıktısı:

- `python3 tools/ci/stage_shared_pc_runtime.py --dest "$HOME/ORUN-SHARED-RUNTIME" --site orun-main --installation pc-main --port 18781 --approve-local-pc` **PASS**.
- `ORUN_SHARED_SIDECAR_STAGED=YES`, `STAGED_SOURCE_COMMIT=264b4117a4d9`.
- Kalıcı dizin `/home/msimsek/ORUN-SHARED-RUNTIME`, sadece `127.0.0.1:18781` aday portu.
- Üç unit dosyası **yalnız** `units/` altına yazılmış; henüz `~/.config/systemd/user` içine kurulmadı. Yeni sistemd servisleri hâlâ çalışmıyor.
- Mevcut gözlem servisleri ve canlı DB değiştirilmedi.

## Açık onayla etkinleştirme — kod eklendi, kullanıcı henüz çalıştırmadı

`tools/ci/activate_shared_pc_runtime.py` yalnız
`--activate` ile:

1. Kalıcı state'in owner-only izinlerini, kaynak SHA-256'larını,
   site/kurulum kimliklerini (`orun-main`/`pc-main`), yeni portu
   ve kaynak koddan üretilmiş unit dosyalarının hedef yollarını doğrular.
2. Mevcut `orun-backend.service`/`orun-pc-edge.service` PID'lerini okur.
   Aynı isimli yeni user unit zaten varsa yazmayı **reddeder**.
3. Sadece üç **yeni** user unit dosyasını `O_EXCL` ve `0600` ile
   yerleştirir; `systemctl --user daemon-reload` yapar (mevcut servisleri
   durdurmaz/restart etmez).
4. Ayrı `orun-shared-sync.service`'i çalıştırıp yalnız 127.0.0.1'de
   PC tokenı ile `/api/v1/sync/identity` doğrular.
5. Salt-okunur PC mirror oneshot çalıştırır; 180 saniyelik
   `orun-shared-pc-mirror.timer`'ı etkinleştirir; iki eski PID'yi karşılaştırır.
6. Herhangi bir adım başarısızsa yalnız bu çağrıda oluşturduğu üç yeni
   unit'i devre dışı bırakıp kaldırmaya çalışır; **özel DB/token/manifesti
   silmez**, eski servisleri asla durdurmaz.

Bu kodun kullanıcı host'unda çalıştırıldığına dair **henüz kanıt yok**;
canlı etkinleştirmeyi başarıyla yapılmış göstermeyin. Kullanıcının açık
`--activate` komutu kurulum ve başlatma onayıdır:

```bash
python3 tools/ci/activate_shared_pc_runtime.py \\
  --staged-dir "$HOME/ORUN-SHARED-RUNTIME" \\
  --site orun-main --installation pc-main --activate
```

Etkinleştirme sonraki hedef için **yerel PC pilotudur**: mevcut gözlem
upload'ı `8090` üzerinde ayrı kalır. Telefon ve başka PC, HTTPS/owner
mobil enrollment tamamlanana kadar bu loopback-only `18781` hizmetine
uzaktan erişemez. Salt-okunur PC tokenı 30 gün sonra bitecek ve
**otomatik yenileme hâlâ yok**; kalıcı üretim erişimi için bu kapı kapanmadı.

## Gerçek sistemi etkinleştirmek için ayrı karar

S4E bir **hazırlama** aşamasıdır. Kullanıcının açık onayı,
doğru site/PC kimliği, token yenileme, 127.0.0.1 port çakışması,
servis yaşam döngüsü ve rollback kontrolü tamamlanmadan canlı
sisteme kurulum yapılmaz.

Gerçek telefon Edge ile ortak mera eşitlemesi ise bundan daha
ileridir: HTTPS ve sahibi onaylı mobil kayıt/KeyStore, Android
SQLite outbox ve UI bağlantısı gerekir. Bu S4E aşaması firmware'e
komut vermez, tasmanın APPLIED durumunu uydurmaz.
