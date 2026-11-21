# PR 357 için yayın öncesi doğrulama

20 Kasım 2026 kontrolünde canlı defterde iki ölçüm zamanı indeksi eksikti. Üretim kabulü açık. Saha denemesine geçiş için aşağıdaki indeks ve sorgu planı kontrolleri gerçek defterde de geçmeli.

## Çalışan rota

- Telefonun Tailscale Serve üzerindeki HTTPS 8443 rotası 127.0.0.1:18781 adresine gidiyordu. Bu portta kullanıcı systemd servisi orun-sunucu çalışıyordu. Çalışan süreçte sürüm dizini sunucu-3862d7d, --db argümanı /home/msimsek/orun-backend-bench.sqlite idi. Dosya adında bench geçmesine rağmen bu aktif defterdi.
- O sürümde hesap HTTP işleyicisinin observation_relay nesnesi aynı süreçteki LocalObservations. latest=1, read_latest(self.backend.db_path) çağırıyor. Eski backend, shared-sync ve observation-readonly servisleri pasifti. Sidecar'ın salt okunur kalması gerekiyor.
- Salt okunur SQLite bağlantısında WAL, 1.856 gözlem ve iki cihaz görüldü. sqlite_master yalnız kimlik birincil anahtar indeksini gösterdi. PR sorgularının ikisinde de EXPLAIN QUERY PLAN cihaz kimliği indeksine ek olarak USE TEMP B-TREE FOR ORDER BY gösterdi.

## Kopya üzerinde süreler

SQLite çevrimiçi backup API'siyle geçici özel dizine alınan WAL tutarlı kopyada PR sorgusu her koşulda aynı kayıtları döndürdü. Her satır 100 latest okumasıdır. Geçmiş yükü, ayrı bağlantılardan kayıtlı imleçle 64'er satır okumaları ve son sayfadan sonra baştan tekrarı içerir.

| İndeks | Geçmiş | Ortanca ms | p95 ms | En uzun ms | Geçmiş sayfası |
|---|---|---:|---:|---:|---:|
| Yok | Kapalı | 14,67 | 21,58 | 26,37 | 0 |
| Yok | Sürekli | 22,77 | 34,51 | 41,71 | 828 |
| Var | Kapalı | 1,69 | 1,95 | 7,58 | 0 |
| Var | Sürekli | 3,14 | 4,41 | 21,97 | 108 |

Ayrı bir indeksli kopyada geçmiş okuyucusu 68 sayfa işlerken, sentetik cihazın 30 yeni konumu tek tek commit edildi. Her biri commit sonrasındaki ilk latest sorgusunda döndü. Ortanca 4,63 ms, p95 28,55 ms, en uzun 30,52 ms idi. Bu ölçüm depolama ve sorgu zamanını kapsar. Radyo, ağ, telefonun zamanlayıcısı ve ekran yenilemesi ayrıca ölçülmelidir. Uygulama testinde açık tutulan bir geçmiş isteği sırasında latest tamamlanıyor, imleç ancak geçmiş sayfası tamamlanınca ilerliyor.

## Onaylı bakım penceresi için sıra

1. Çalışan servis PID'sini, --db yolunu ve Serve hedefini yeniden doğrula. Boş disk alanını kontrol et. İndeks oluşturma SQLite yazıcı kilidini alır. Yayın, indeks işlemi ve varsa hizmet durdurma/başlatma için operatörden açık onay al.
2. Kurulu yedek aracıyla özel, yeni bir hedef dizine yedek al. Örneğin proje kökünden `python3 tools/orun_yedek.py al --hedef "$YEDEK_HEDEFI"`. Üretilen yedek klasörünü kullanarak `python3 tools/orun_yedek.py prova --yedek "$YEDEK_KLASORU"` çalıştır. Manifest, bütünlük ve dönüş provası başarılı olmalı. Açık defterin yalnız ana dosyasını kopyalamak WAL içindeki kayıtları atlayabilir.
3. Önce yedeğin ayrı çalışma kopyasında, sonra açıkça onaylanan gerçek defterde aşağıdaki işlemi uygula. Komutu PR kaynak kökünden, DB ortam değişkenini doğrulanmış mutlak dosya yoluna ayarlayarak çalıştır. Arşiv kaydı, cursor, ACK ve teslim tablolarına yazma. Yazıcı kilidi alınamazsa işlemi ertele.

```python
import os
import sqlite3
from pathlib import Path
from contextlib import closing
from backend.observation_read_page import LATEST_INDEXES, _LOCATION, _REPORT, _STAMP

path = Path(os.environ["DB"]).resolve(strict=True)
with closing(sqlite3.connect(path.as_uri() + "?mode=rw", uri=True, timeout=2)) as db:
    with db:
        db.execute("BEGIN IMMEDIATE")
        for ddl in LATEST_INDEXES:
            db.execute(ddl)
        for name, predicate in (("latest_location_utc", _LOCATION),
                                ("latest_report_utc", _REPORT)):
            plan = db.execute(
                f"EXPLAIN QUERY PLAN SELECT {_STAMP},rowid FROM verified_observations "
                f"WHERE device_id=? AND {predicate} AND {_STAMP}>=? "
                f"ORDER BY {_STAMP} DESC,rowid DESC LIMIT 1",
                (bytes(8), bytes(4))).fetchall()
            detail = " ".join(row[-1] for row in plan)
            print(name, detail)
            if name not in detail or "TEMP B-TREE" in detail:
                raise RuntimeError("Beklenen indeks planı bulunamadı")
```

4. Commit sonrasında ayrı bir mode=ro bağlantısından sqlite_master ve her iki sorgu planını yeniden oku. İki planda da ilgili indeks adı ve geçici sıralamanın kalktığı görülmeli. Başarısız işlemde transaction geri alınır. Üretime sonradan ulaşan kayıtları korumak için eski yedeği çalışan defterin üzerine yazma.
5. Onaylı sunucu sürümü ile saha deneme telefonunda geçmiş indirilirken bir yeni GPS ve ardından zaman damgalı bir GPS'siz rapor izle. Ölçüm, Edge varışı, sunucu varışı ve ekranda görünme zamanlarını ayrı kaydet. Yeni GPS ilk başarılı latest kontrolünde görünmeli. Ardından son rapor ilerlemeli, GPS yaşı kendi ölçümüne bağlı kalmalı. Uygulamayı arka plana alıp geri açınca aynı davranışı, ilerleyen geçmiş imlecini ve normal toplu ACK akışını doğrula. Bunlar gerçekleşmeden genel yayına geçme.
