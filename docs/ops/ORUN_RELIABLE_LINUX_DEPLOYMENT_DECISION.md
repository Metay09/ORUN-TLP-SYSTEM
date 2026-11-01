# ORUN Linux kurulumu — güvenilir işletim kararı (2026-10-28)

**Durum:** Uygulama/kurulum yönü; üretim sertifikasyonu değildir. Bu belge mevcut çalışan sistemi değiştirmez.

## Karar

Yeni bir PC'de güvenilir kurulum için **hibrit** kurulum kullanılacak:

1. **Gateway USB -> PC Edge:** Debian/Ubuntu **ana işletim sistemi** üzerinde `systemd --user` servisi. Doğrudan `/dev/serial/by-id` cihaz erişimi; yerel SQLite spool. Şu anda doğrulanmış tek gerçek aktarım taşıyıcısı USB'dir. PC Edge USB'yi sırf Docker'a geçmek için taşımayacağız.
2. **PC Edge -> Backend:** Tailscale üzerinde özel ağ, bearer-token dosyası (0600). Backend aynı Linux PC'de veya ayrı Linux sunucuda olabilir. Backend yoksa Edge yerel kuyruğu tutar; `STORED/DUPLICATE` olmadan teslim onayı yok.
3. **Backend:** İlk etapta mevcut `orun-backend.service` korunacak. **Yeni ve bağımsız** Linux backend kurulumları için, uygun testlerden sonra Docker Compose paketlemesi hazırlanabilir. Backend Docker'a geçişi mevcut veritabanının güvenli taşınması/tek yazıcılık/doğrulama kapılarına bağlıdır.
4. **BlueZ/BLE:** Linux ana makinesinde. Sorunlu Debian 12 BlueZ 5.66'da `bluetoothd` `SIGABRT` / `gatt-db.c:371 db_hash_update` görülmüştür. Docker bu host hata sınıfını düzeltmez. BLE, yeni kayıt -> backend kabul -> Gateway onayı gerçek donanımda sınanana kadar varsayılan taşıyıcı olmayacak.
5. **Gizli bilgiler:** Tracker root key/registry yalnızca yetkili backend tarafında; PC Edge bunlara erişmez. Token ve kayıtlar Git'e veya Docker image'ına eklenmez. Kayıtların yedeği SQLite'ın tutarlı `.backup` yöntemiyle alınır; çalışan dosyanın sadece `.db` kopyalanması WAL ile tutarsız olabilir.
6. **Güvenlik:** Şu anki SF5L0 `--bench-release-gateway` HMAC protokolü PUBLIC TEST KEY'dir, üretim yetkilendirmesi değildir. Başka bir sistemde üretim/provisioned Gateway'e yanlışlıkla etkinleştirilmez. Backend'in `orun_backend_bench.py` sunucusu da **bench** statüsündedir; üretim sertifikasyonu iddia edilmez.

## Başlatma ve yeniden açılma

- Host PC Edge: `tools/ops/orun_pc_edge_service.sh` mevcut servis arayüzü; otomatik başlatma için kullanıcı linger (`loginctl enable-linger`) ve systemd unit. Kurulumdan sonra servis/USB bağlantısı/SQLite spool kontrol edilir.
- Backend: Mevcut sistemd host kurulumu zaten çalışıyor. İleride Compose eklenirse `restart: unless-stopped`, sağlık kontrolü, yalnızca özel Tailscale arayüzünde dinleme, değişmez image etiketi, `read_only` uygun dizinler, sınırlı mount ve dışarıda kalıcı DB/registry/token sağlanmalı.
- USB cihazı değişince `/dev/serial/by-id` yolu tekrar doğrulanmalı; rastgele `ttyACM0` varsayılmamalı.
- Aynı Gateway için **tek** aktif PC Edge veri alıcısı; aynı SQLite için **tek** yazıcı sahibi. Systemd + Docker paralel çalıştırılmaz.
- Backend'e ulaşılamazsa **kayıt kaybetmeden** spool/retry uygulanır. Yeni backend'e göç öncesinde SQLite bütünlük, satır sayısı, uçtan uca mutabakat ve geri dönüş planı doğrulanır.

## Kabul kapıları

1. Mevcut host USB -> PC Edge -> Backend sahası: **101/101 STORED + Gateway bench accept 101/101** daha önce doğrulandı; bu PUBLIC TEST KEY bench sınırıdır.
2. USB PC Edge ve Backend sürekli çalışan host servisleri: sahada `active`. Bu, reboot/network kesintisi gibi tüm senaryoların denendiği anlamına gelmez.
3. Yeni bir Linux PC'ye kurulum: `systemd` servisleri, Tailscale erişimi, token, kalıcı SQLite, `/dev/serial/by-id` ve yeni gerçek Gateway kayıt akışı ayrıca doğrulanacak. Sadece 14/14 host Python testinden fiziksel PASS çıkarılmaz.
4. Docker backend: henüz kurulmadı ve doğrulanmadı. Compose dosyaları eklendiğinde bile mevcut host servisi durdurulmadan ve veritabanı doğrulanmadan canlı geçiş **yok**.
5. BLE: BlueZ sorunu giderilene ve gerçek HELLO/DUMP + yeni record/ACK zinciri geçene kadar **beklemede**.

## Dağıtım topolojileri

- **A — Tek PC:** Gateway -(USB)-> Linux PC Edge (host systemd) -> Backend (ayrı servis; ileride Docker) -> SQLite kalıcı veri.
- **B — Ayrı backend:** Gateway -(USB)-> Linux PC Edge (host systemd) -> Tailscale özel ağı -> Ayrı Linux Backend -> kalıcı ledger + credential registry.

**Karar gerekçesi:** Taşıyıcıyı Docker'a almak bugünkü BlueZ sorununu çözmüyor, USB cihaz yetkilerini ve geri dönüş riskini artırıyor. İlk önce kullanıcıya gerçekten çalışan USB hattı korunacak; Docker, asıl tekrarlanabilir dağıtım değeri sunduğu backend tarafına kontrollü uygulanacak.
