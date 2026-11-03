# Y2a — tek tasma, tek gateway yazılımı (deneme anahtarı cihazda yok)

**Durum:** kod + host testleri. Fiziksel yükleme: sahibin PC'sinde `bash tools/orun_cihaz_yukle.sh`.
Plan: `docs/product/ORUN_UYGULAMA_KAPSAMI.md` (Y2a, Y2b).

## Ne değişti

| | Önce | Y2a |
| --- | --- | --- |
| Tasma yazılımı | `rak4630_sf5l0_tracker_bench` (deneme anahtarlı) ve `rak4630_phase2_tracker_real_pilot` | yalnız `rak4630_tracker` |
| Gateway yazılımı | `rak4630_sf5l0_gateway_bench` | `rak4630_gateway` |
| Tasmaya deneme kimliği (`BENCH SETUP <gateway>`) | yazılabiliyordu | reddedilir; yalnız kendi anahtarı (`orun_provision.py tracker` → `BENCH PROVISION`) |
| Gateway'in kayıt kabulü | imzalı izin → RAM izni → **deneme anahtarı** | imzalı izin → RAM izni; deneme anahtarlı tasma reddedilir (kayıtları tasmada kalır) |
| Tasma yazılımında deneme kökü | vardı | yok (host testi nesne dosyasında arar) |
| Gateway'in teslim onayı (EDGE ACCEPT) anahtarı | deneme kökünden | **değişmedi** — Y2b |

- USB komut adları (`BENCH?`, `BENCH PROVISION`, `BENCH DRAIN …`) araçlar bozulmasın diye aynı kaldı; "BENCH" artık deneme anahtarı demek değil.
- Eski deneme kimliği olan bir tasma `BENCH?` satırında `test_credential=yes` der (yalnız açık kimlik ve dönemden; kök gerekmez). `tools/orun_cihaz_yukle.sh` böyle bir tasmaya yüklemeden durur.
- `ORUN_SF5L0_BENCH_TEST_KEYS` yalnız host testlerinde; `main.cpp` bu bayrakla derlenmeyi `#error` ile durdurur.
- Ayrı faz 2 tasma kaydı (`TrackerRealEnrollment`) kaldırıldı; gerçek anahtar yolu tek: `Sf5l0BenchTestProvisioning::startWithCredential`.
- Sürüm kapısı (`tools/ci/orun_product_release_gate.py`): yeni `product-images-no-test-key` kontrolü.

Flaş düzeni, kayıt biçimleri, telsiz ve uygulama değişmedi: yükleme kayıtları ve anahtarları korur.

## Denendi

- `tools/ci/run_y2a_product_keys_host.sh` (yeni, ASan/UBSan): ürün gibi derlenen kod — gerçek anahtarla kayıt ve gateway izni, deneme anahtarlı tasmanın tanınması ve gateway'in reddi (RAM izni olsa bile), gerçek doğrulayıcıyla ACK; tasma derlemesinde `kTestRoot` ve deneme kurulum girişi yok, gateway derlemesinde deneme kurulum girişi yok.
- Eski host testleri (deneme anahtarıyla, yalnız host): SF5L0, 3A, 3B, SF5L5, SF5L6, SF5L0 temizleme — PASS.
- `backend/tests/test_cihaz_yukle.py`, `firmware/tests/sf5/test_orun_product_release_gate.py` — PASS.
- Cihaz yazılımı bu ortamda derlenemiyor; ilk derleme sahibin PC'sinde.

## Sahada sıra

1. Tasma: `bash tools/orun_cihaz_yukle.sh` → tasmayı seç. Sonuç `TAMAM` satırları; yazılım `0.5-<sürüm>`.
2. Gateway: aynı komut → gateway'i seç (PC Edge yükleme boyunca durur). Ardından tasmanın raporu gateway'e ulaşmalı ve uygulamada görünmeli.
