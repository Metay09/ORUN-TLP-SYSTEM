# Testler PC'de çalıştırılır

Sahip kararı — 12 Kasım 2026: GitHub Actions test iş akışları kaldırıldı.
Test kaynakları ve yerel çalıştırma betikleri korunur. Yeni Actions iş akışı
ancak sahibin açık kararıyla eklenir. Eski dallardan iş akışlarını geri getirmeyin.

Aşağıdaki komutlar depo kökünde, gereken geliştirme bağımlılıkları kurulu PC'de
çalıştırılır. Her değişiklikte ilgili testler seçilir; sonuçla birlikte
`git rev-parse HEAD` çıktısı ve kullanılan komutlar kaydedilir.

| Kaldırılan GitHub iş akışı | PC'deki mevcut karşılığı |
| --- | --- |
| Edge queue telemetry | `bash tools/ci/run_edge_queue_telemetry_host.sh` |
| Product release gate regression | `python3 firmware/tests/sf5/test_orun_product_release_gate.py -v` |
| SF5K3A ObservationStore | `bash tools/ci/run_sf5k3a_host.sh` |

Edge queue betiği için Node.js 22, Python 3.12 ve uygulama bağımlılıkları gerekir:
`cd edge/app && npm ci --no-audit --no-fund`; Python tarafında
`cryptography>=41,<47`. Host C++ kontrolleri için GCC ve sanitizer desteği gerekir.

`python3 tools/ci/orun_product_release_gate.py --json` ayrıca çalıştırılabilir:
ürün hazır değilse sıfır dışı çıkış beklenir; bu sonuç testlerin geçtiği veya
fiziksel ürün kabulünün tamamlandığı anlamına gelmez. Eski workflow'daki belirli
blokajların mutlaka kalmasını isteyen sabit beklenti yeni yerel şart değildir.

Bu değişiklik testleri çalıştırmaz; test zorunluluğunu kaldırmaz. Firmware,
uygulama ve sunucu testleri ile gereken fiziksel kabul kontrolleri PC üzerinden
sürdürülür. Önceden oluşmuş GitHub koşu kayıtları silinmez.
