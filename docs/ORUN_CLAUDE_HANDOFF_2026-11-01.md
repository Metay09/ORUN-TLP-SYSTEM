# ORUN — Claude'a teknik devir / 1 Kasım 2026

**Durum: KISMİ PİLOT BAŞARILI, ÜRÜN HENÜZ TAMAMLANMADI.**
Bu belge, sahibinin talebi üzerine GitHub'da yapılan gerçek teslimdir. Kullanıcıyı yeni uzun test döngülerine sokmak yerine mevcut çalışan hattı koruyarak öncelikle **en güncel gerçek konumu** göstermeye odaklanın.

**Kaynak / PR:** `Metay09/ORUN-TLP-SYSTEM`, taslak **PR #227**, branch `feat/app-durable-observation-mirror-20261101`; bu belgeden önceki kod HEAD **a300b8e**. PR **merge edilmedi**; GitHub Actions sonucuna güvenmeyin. GitHub dalında bu çalışma sürecinde oluşturulan kod ve testler zaten var, eksik olan iş güvenilir ürün davranışı.

## 1. Sahadan kullanıcının doğrudan geri bildirimi — EN KRİTİK

1 Kasım 2026, Samsung S23 üzerinde gerçek debug APK ile kullanıcı `owner` hesabına giriş yaptı. **Ağ → Sunucudan konumları al** tek basış: **`256 kayıt kontrol edildi · 4 sayfa · Daha eski kayıtlar var, yeniden al`**. Telefon Gateway/istasyona BLE/USB ile bağlı değildi. Ağ ekranında bu telefondan alınmış Gateway kayıt sayacı 0; sunucu bağlantısı doğrulanmıştı. Haritada `Tasma 1AA3` görüldü fakat **`2 gündür haber yok · kayıp olabilir`** mesajı ve **çok eski konum** gösterildi. Kullanıcı açıkça **“Çok önceki verilerden geliyor”** dedi.

Bu başarıyı *canlı/güncel konum testinden geçildi* diye raporlamayın. **Temel hesap → sunucu kayıtları → Android kalıcı ayna → harita entegrasyonu çalıştığına güçlü kanıt vardır; en güncel kayıt gösterimi başarısızdır.** O anda Backend'deki en son fiziksel konumun yaşı bilinmiyor; bunun ölçüm mü, varış mı, telefon arşivi mi olduğu ayrı değerlendirilmelidir. Ekran üzerindeki “kayıp olabilir” uyarması en yeni veriye ulaşılmadan yanlış alarm verebilir.

**Kod düzeyi kök neden (doğrulandı):**
- `backend/observation_read_page.py::read_page()`: sorgu `WHERE o.rowid > ? ORDER BY o.rowid ASC LIMIT 65`; her sayfa 64 kayıt, cursor **sunucu SQLite ekleme sırası**, **GPS ölçüm zamanı değildir**.
- `edge/app/src/app/accountObservationMirror.ts`: `MAX_PAGES=4`, `pullAvailable()` yalnız 4×64=256 kayıt çekiyor; ilk defa `cursor=0` ile başlıyor, dolayısıyla eski arşivden geliyor. Manüel olarak tekrar basmak gerekiyor.
- `edge/app/src/features/network/NetworkScreen.tsx`: buton `Sunucudan konumları al`; yalnızca kayıt/sayfa sayısı ve `more` metni gösteriyor, arşiv gerisinde olunduğunu ve gerçek güncellik/freshness doğrulanmadığını yeterince vurgulamıyor.
- `edge/app/src/app/controller.ts::points()` yerel `core.points()` ile hesap `pilotAccountObservationPoints()` verisini birleştiriyor; Gateway yerel ACK sayacına yazmıyor.
- `edge/app/src/app/store.ts` bu birleşik noktalardan `buildHerd()` ile sürü durumunu çıkarıyor. Eski indirme tam bitmeden yeni konuma dair güvenilir “güncel/kayıp” yorumu yapılamaz.
- `edge/app/src/app/accountObservationMirror.ts` SQLite cursor ve kayıtları account/site/installation kapsamına göre atomik saklıyor; her frame telefonda AEAD ile yeniden doğrulanıyor. **Bu güvenlik ve kalıcılık özelliklerini bozmayın.**

**Sıradaki gerçek ürün görevi:** Baştan binlerce eski kayıt indirmeden **en son doğrulanmış tasma konumunu öncelikle göstermek**, sonrasında geçmişi sınırlı/iptal edilebilir ve kesildiği yerden devam edebilir biçimde arka planda tamamlamak. Latest-first API gerekiyorsa, server yetkisi + site sınırı + AEAD doğrulaması + zaman dürüstlüğü + takılı cursor/eksik sayfa hata koşulları korunmalı. Alternatif hızlı çözüm tam arşiv indirmeyi otomatik, ilerleme göstergeli ve kesilebilir yapabilir ama son konuma ulaşma gecikmesini azaltmıyorsa ürün ihtiyacını karşılamaz. **Ölçüm zamanı, sunucuya ulaşma zamanı ve veri yeniliği ayrı gösterilmeli.** Veritabanına son kayıttan ilk kayda `rowid` ters okunması tek başına güvenilir “en son GPS ölçümü” anlamına gelmez. Cihazların farklı report periyotları ve delayed store-and-forward göz önünde tutulmalı.

**Net kabul:** Telefon BLE/USB bağlantısızken oturum açılır; gerçek tasma için sunucunun gerçekten sahip olduğu **en yeni doğrulanmış GPS ölçümü** ve **ölçüm tarihi** gösterilir, sunucuya **geliş tarihi** ayrı gösterilir, stale/offline nedeni açıklanır. Uygulama sadece 256 eski kayıt gösterdiği halde “hayvan kayıp” sonucuna atlamaz. Toplam kayıt sayısı arttığında da işlev sürer.

## 2. Gerçekten kurulan çalışan sistem (kullanıcı terminal çıktıları)

**Donanım / ağ:** Gateway USB üzerinden PC Edge'e bağlı; telefon S23. `site=orun-main`, `installation=pc-main` (önceden kaydedilen kurulum bağlamı). Tailscale hostname `simsek.tail823510.ts.net`, bilgisayar tailnet IP `100.114.36.9`, telefon bir kontrolde `100.65.103.103` (değişebilir). PC `tailscale set --accept-dns=true` sonrasında MagicDNS / genel DNS / HTTPS **PASS**. Tailscale Serve:
- **`:8443` HTTPS** → `127.0.0.1:18781` hesap servisi; `GET /api/v1/account/me` hesapsız **401** beklendi ve alındı.
- **`:8090`** mevcut Backend, `100.114.36.9:8090`; gerçek `~/orun-backend-bench.sqlite` verified ledger. PC Edge bu Backend'e veri gönderiyor, **Android olmadan da** çalışıyor.
- **`:18782`** `orun-observation-readonly.service`, yalnız `127.0.0.1`: salt-okunur ayrı HTTP servis. Canlı test: hesapsız `GET /v1/edge/records?after=0` → 401; yetkili → 200, **64 gerçek kayıt**; POST `/v1/edge/batch` → 405.
- **`:18781`** `orun-shared-sync.service`, `127.0.0.1`: yeni hesap köprüsü. `--observation-upstream http://100.114.36.9:8090` (mevcut Backend, keys+upload) ve `--observation-records-upstream http://127.0.0.1:18782` (GET records); `--observation-site orun-main`. Sahibi gerçek hesapla Android giriş yaptı.
- `orun-backend.service`, `orun-pc-edge.service`, `orun-shared-sync.service`, `orun-observation-readonly.service` kullanıcı çıktısında **dördü de active**. Bu, yalnızca o kontrol anı içindir; canlı durum tekrar okunmalı.
- `:3050` Tailscale Serve default HTTPS / PC web hizmeti; `127.0.0.1:3050/api/points` 404 olduğu görüldü; Backend harita hizmeti farklı.

**Sunucu staging ve systemd (sahibin PC'sinde, dosyalar GitHub'da değil):**
- `~/ORUN-APP-VERIFY-20261101`: temiz Git verify tree; ilk dağıtım HEAD **a300b8e**.
- `~/ORUN-RELEASES/observation-7dcd44b`: ilk stage; **yerine doğrudan geçilmedi**.
- `~/ORUN-RELEASES/observation-a300b8e`: sidecar / hesap köprüsünün canlı yürüttüğü kaynak kod.
- `~/.config/systemd/user/orun-observation-readonly.service`: `--db ~/orun-backend-bench.sqlite`, `--token-file <backend-özel-token-dosyası>`, `--bind 127.0.0.1 --port 18782`. Restart on failure, enabled.
- `~/.config/systemd/user/orun-shared-sync.service.d/99-observation-bridge.conf`: `ExecStart` override; yeni runtime `~/ORUN-RELEASES/observation-a300b8e/tools/orun_shared_sync_loopback.py`, state `~/ORUN-SHARED-RUNTIME/state/backend`, `--reuse-state --enable-account-login`, `--port 18781`, yukarıdaki iki upstream + pinned site. Eski Backend/PC Edge için restart yapılmadı.
- **Account SQLite online yedeği:** `~/ORUN-RELEASES/account-prebridge.YJcIwThf/baseline` içinde `shared_resources.sqlite`, `enrolled_edges.sqlite`, `owner_accounts.sqlite`; bu **private yerel yedektir**, GitHub'a yüklenmemeli. Shadow DB üzerinde yeni account daemon startup smoke PASS.
- Token dosyası canlı Backend process argümanlarından keşfedildi ve **0600/private** permission kontrolü PASS. **Token içeriği hiçbir yerde kopyalanmamalı, dökülmemeli, GitHub'a yazılmamalı.**
- Backend verified ledger test anında **3.605 kayıt** (sonradan artmış olabilir). Testte gerçek SQLite **read-only** kullanıldı; kullanıcı istediği için eski test kayıtları daha önce sıfırlanmıştı. **Artık veritabanını silmeyin.**
- Eski Backend **`GET /api/points` hesapsız 200** ve onun bağımsız web haritası bunu kullanıyor; bu **gerçek bir konum gizliliği sorunu**. Yeni PR Backend dosyasında bu uç nokta bearer ister; ama bu kodu mevcut `:8090` servisine doğrudan geçirmek eski haritayı bozacağından uygulanmadı. Yeni sidecar çözümü iki servisi ayırarak veri akışını korudu; eski endpoint'i **güvenli hale getirmedi**. Üretim öncesi harita yetkili hale getirilmeli; mevcut tailnet açılımı ayrıca denetlenmeli.

**APK / mobil:**
- Sürüm **`a300b8e`** `VITE_ORUN_ACCOUNT_ORIGIN=https://simsek.tail823510.ts.net:8443 npm run android` + Gradle `assembleDebug` → **PASS**.
- Bilgisayardaki APK `~/ORUN-RELEASES/APK/ORUN-account-observation-a300b8e-debug.apk`, ~16 MB; SHA-256 **`18b95cf0e71d57ee0a905bea7d70a1cf6db63702882ccb0135e7951c3b373ddb`**. Bu **debug**, production-release değildir, GitHub'a binary yüklenmedi.
- Taildrop, SCP/SFTP ve SSH stream aktarımı başarısız oldu; Python HTTP sunucusu üzerinden Tailscale'e açık özel PC adresinden telefon indirmesi yapıldı. Daha önce çalışan basit Python yöntemini tercih edin; tekrar tekrar karmaşık aktarım denemesi yaptırmayın. Dosya sunucusunun hâlâ çalıştığı **bilinmiyor**; gerekiyorsa kapatın, kalıcı internet/public servis açmayın.
- Android **Ayarlar → Hesabım** login, **Ağ → Sunucudan konumları al** manual, **Harita** görünümü kullanıcı ekranlarında doğrulandı. Android hesap API hesabın şifresini JS belleğinde saklamaz; token kısa ömürlüdür.
- Android `edge/app/capacitor.config.json`: `CapacitorSQLite.androidIsEncryption=false`; ayna depolamasında doğrulanmış lokasyon plaintext hex mevcut. Üretim veri koruması / cihaz kaybı senaryosu **açık güvenlik kapısı**.

## 3. Repo kod haritası

- `backend/observation_read_page.py`: read-only cursor-bounded 64 records/response, `verified_observations` + `edge_receipts`.
- `tools/orun_observation_readonly_sidecar.py`: yalnız 127.0.0.1, bearer, GET records; POST 405, key yok, map yok.
- `backend/account_observation_relay.py`: ayrı `records_upstream` ile records sidecar'a, keys ve batch eski Backend'e; private token ve one-site pin.
- `tools/orun_shared_sync_loopback.py`: runtime + CLI; `--observation-{upstream,token-file,site,records-upstream}`.
- `backend/account_login_http.py`: gerçek hesap girişi, `zone:read` vs `zone:write` scope; site pin. Token ve yetki ihlali olarak ham anahtarı telefona göndermek yanlış hesapta engellenmeli.
- `backend/orun_backend_bench.py`: yeni sürüm auth-protected `/api/points` davranışı **kodda var**, fakat eski canlı Backend bununla değiştirilmedi.
- `edge/app/src/app/ownerAccountPilotRuntime.ts`: oturum ve opt-in konum indirme.
- `edge/app/src/app/accountObservationMirror.ts`: 4×64, SQLite kalıcılık, per-frame AEAD, cursor ve offline points. **En yeni veriye ulaşma UX problemi burada.**
- `edge/app/src/app/serverObservationRead.ts`: sayfa şema ve cursor doğrulama.
- `edge/app/src/app/controller.ts`: `new KeyRing(false)` production controller; `points()` local+server merge.
- `edge/app/src/features/network/NetworkScreen.tsx`: manuel konum indirme düğmesi ve sonucu.
- `edge/app/src/features/settings/AccountLoginPanel.tsx`: kullanıcı adı/parola; başka teknik token/adres yazdırmıyor.
- `edge/app/src/app/orunServiceConfig.ts`: tailnet HTTPS hesap origin config.
- `edge/app/test/accountObservationMirror.test.ts`: mobil sahte ledger/AEAD ve account mirror regresyon testleri.
- `backend/tests/test_observation_read_page.py`, `test_observation_account_relay_e2e.py`, `test_observation_readonly_sidecar.py`: cursor/relay gerçek SQLite read-only/sidecar testleri.

## 4. Bilinen testler, bilmediğimiz şeyler

- HEAD `7dcd44b` üzerinde **19/19 Python**, **118/118 Node app**, TypeScript ve Vite build PASS.
- HEAD `a300b8e` üzerinde Python syntax + **23/23 Python** (gerçek verified SQLite read-only opt-in dahil) PASS; Android Capacitor ve Gradle debug APK build PASS. Android TypeScript kodu önceki 118 Node testinden sonra değişmedi. **CI / GitHub Actions çalışıyor diye iddia edilmedi.**
- S23'te gerçek `owner` girişi, 256 kayıt transferi, haritada gerçek cihaz `Tasma 1AA3` kaydı görüldü.
- **Bilinmeyen / yapılmayan:** Son GPS ölçümünün gerçekten en güncel kayıtla eşleşmesi; otomatik backfill; gerçek sahada tazelik ve yanlış kayıp alarmı; firmware/GNSS/LoRa uçtan uca dış saha; Android data-at-rest encryption; production security; full owner multi-site/role isolation; web harita auth migration.
- Son servis durumu değişmiş olabilir; önce `systemctl --user is-active`, hiçbir şeyi rastgele restart/silmeyin.
- Tasma donanımı RAK4630 / nRF52840 firmware işini ve tüm mevcut SF aşamalarını çöpe atmayın. Başka repo veya sıfırdan yazma önerisi yok; amaç mevcut işleyen altyapıyı ürünleştirmek.

## 5. Claude için somut devralma sırası

1. **Hiçbir hizmeti değiştirmeden** PR #227 ve bu belgedeki gerçek Android ekran sonucunu incele. İlk iş, Android uygulamasının neden eski kayıtları öne çıkardığını çöz. Son ölçüm/geliş zamanını kesin ayır; sunucunun doğrulanmış en yeni GPS ölçümünü belirlemek için site/cihaz bazlı yetkili API tasarla veya güvenli mevcut tarihsel backfill'i otomatik/ilerlemeli yap. Sadece `rowid DESC` = son GPS varsayımı yapma; ölçüm zamanı ve kayıp uyarısı dürüst olsun.
2. Tüm tasmalarda latest-point/atomic-cursor/AEAD/site isolation testleri; birden fazla cihaz ve geç gelen eski çerçeveler, offline ve yarıda kalan aktarım dahil. Uygulamada son güncelleme ve eksik arşiv ayrımı.
3. Kullanıcıyı gereksiz tekrar komutlarına sokmadan kontrollü tek debug APK üret; **önce gerçekten gereken UI/işlev düzeltmeleri biriktir**, sonra test et.
4. Gerekli ise `orun-shared-sync.service` / `orun-observation-readonly.service` için küçük, rollback'li değişiklik yap; **Backend ve PC Edge'i koru**. Canlı USB/Gateway kayıt kuyruğuna dokunma.
5. Üretim öncesi açık güvenlik kapıları: legacy `/api/points` anonim koordinat, Android SQLite encryption=false, hesap anahtarları ve yetki ayrımı, multi-site ayırımı. Gerçek saha hazır denmeden bunlar ele alınmalı.

**Kullanıcının beklentisi:** “Güncel hayvan konumu telefon Gateway'e bağlanmadan görünsün. Tek komutla çalışsın, eski verileri güncelmiş gibi göstermesin. Her küçük değişiklikte yeni APK çıkarma. Sürekli test döngüsü ve yeni mimari icat etme.”

## 6. Koruma / yasaklar

- Gerçek şifreleri, token dosyası içeriklerini, credentials registry'yi, özel cihaz trafik anahtarlarını, canlı SQLite veya özel konum koordinatlarını GitHub'a yüklemeyin.
- `orun-backend.service`, `orun-pc-edge.service`, `orun-shared-sync.service`, `orun-observation-readonly.service` rastgele kapatılmamalı/silinmemeli. Geri dönüş için mevcut `99-observation-bridge.conf` ve account baseline konumları yukarıda.
- Debug APK cihazda mevcut SQLite uygulama verisini barındırabilir; önceki uygulamayı uninstall ettirmeyin.
- Kullanıcı gerçek konuma ulaşılamadığı halde “tamamlandı / güncel / kayıp” şeklinde yanlış kesinlik istemiyor.
- Kullanıcının artık Claude ile devam etme kararı var; bu belge **daha fazla ChatGPT komutu gerektirmeyen bağımsız devir** olarak hazırlanmıştır.
