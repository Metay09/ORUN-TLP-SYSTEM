# Y2a — tek ORUN yazılımı (rol cihazda; deneme anahtarı cihazda kayıt yolu değil)

**Durum:** kod + host testleri + ARM derleme denetimi (clang, aşağıda). Fiziksel yükleme: sahibin PC'sinde `bash tools/orun_cihaz_yukle.sh`.
Plan: `docs/product/ORUN_UYGULAMA_KAPSAMI.md` (Y2a, Y2a-2, Y2b). Sahip kararı 2026-11-03: tek yazılım, rol kurulumda; boş cihazda kurulumda sor, GPS'e göre öner.

## Ne değişti

| | Önce | Y2a |
| --- | --- | --- |
| Yazılım | `rak4630_sf5l0_tracker_bench`, `rak4630_sf5l0_gateway_bench`, `rak4630_phase2_tracker_real_pilot` | yalnız `rak4630_orun` (her cihaza aynı) |
| Tasma mı gateway mi | derlemede seçilirdi | açılışta, bölgeleri açmadan önce (`device_storage_role`) |
| Tasmaya deneme kimliği (`BENCH SETUP <gateway>`) | yazılıyordu | reddedilir; yalnız kendi anahtarı (`BENCH PROVISION`) |
| Gateway'in kayıt kabulü | imzalı izin → RAM → **deneme anahtarı** | imzalı izin → RAM; deneme anahtarlı tasma reddedilir (kayıtları tasmada kalır) |
| Teslim onayı (EDGE ACCEPT) anahtarı | deneme kökünden | **değişmedi** — Y2b (deneme kökü yalnız bunun için yazılımda) |

### Rol kararı (açılışta, SoftDevice'tan önce, yalnız okuma)

Aynı iki bölge iki rolde farklı biçimde kullanılır: güvenlik bölgesi (tasmada "ORS1" anahtar sayfaları, gateway'de "OGK1" imzalı izin) ve kayıt bölgesi (tasmada ObservationStore, gateway'de OCQ2 emanet deposu).

1. Yazılı olan karar verir: ORS1 ya da tasma kaydı → **TRACKER**; OGK1 ya da emanet sayfası → **GATEWAY**. Her depo diğerinin sayfalarını zaten kendisi reddeder; karar yalnız boş bölgede önem taşır.
2. İki bölge de boşsa kayıtlı hizmet seçimi: `APP SERVICES A` → gateway; `T`, `R`, `TR`, `NONE` → tasma.
3. Seçim de yoksa (AUTO) **UNASSIGNED**: hiçbir depo açılmaz, hiçbir şey yazılmaz, konum takibi çalışmaz. `BENCH?` yalnız `BENCH DEVICE … role=UNASSIGNED … gnss=yes|no` der; anahtar taşıyan kurulum satırları reddedilip silinir. Rol seçilince (`APP SERVICES T|A`) cihaz yanıtını verip 3 sn sonra o rolde yeniden başlar.
4. İki rolün verisi birden → **CONFLICT**: hiçbir depo açılmaz, hiçbir şey yazılmaz.

- Kurulmuş cihaza öbür rolün hizmeti yazılmaz (`APP SET … code=INVALID reason=device_is_…`); rol ayarla sessizce değişmez (AGENTS.md: kaydedilmiş seçim sessizce AUTO'ya düşmez).
- Gateway rolünde hizmet hep "A" (alıcı, BASE taşıyıcı); BLE hep açık (gateway köprü sözü). Tasma rolünde pilot TX, boşaltma hızı ve bağlantı satırları eskisi gibi.
- `BENCH?` her rolde `BENCH DEVICE id=… role=… security_region=… gnss=…` satırıyla başlar; eski satırlar (`BENCH TRACKER`, `BENCH GATEWAY`) aynı.
- PC: `tools/orun_cihaz_yukle.sh` her cihaza `rak4630_orun` yükler (her derlemede `~/orun-issuer.pub` gerekir); kurulmamış cihaza da yükler, çelişkili cihaza yüklemez; deneme anahtarlı tasmaya yüklemeden durur. `tools/orun_cihaz_kontrol.py rol --port P tasma|gateway` yalnız kurulmamış cihaza rol verir (GPS uyarısıyla).
- Uygulama (kurulumda rol sorusu): boş cihaz USB'ye takılınca Sürü → "+" ve Cihazlar → "USB ile bağlı cihaza bağlan" "Tasma mı, gateway mi?" diye sorar (`RoleChoice`). GPS takılıysa tasma, yoksa gateway "önerilen". Seçim `APP SERVICES T|A` ile yazılır; cihaz "ROLE chosen" deyip yeniden başlar; uygulama USB'yi yeniden açar (Android izni yeniden sorabilir), cihazın seçilen rolde açıldığını doğrular. Ardından tasma hesaba eklenir (mevcut kurulum), gateway bu telefona köprü olarak bağlanır. Rolü belli cihaz seçimi reddeder ("rolü zaten belli"). Çelişkili cihaz açık bir mesajla söylenir; canlı kart (TrackerLive) kurulmamış cihazı Cihazlar'a yönlendirir.
- `ORUN_SF5L0_BENCH_TEST_KEYS` yalnız host testlerinde (`main.cpp` `#error`). Ayrı faz 2 kaydı (`TrackerRealEnrollment`) kaldırıldı; gerçek anahtar yolu tek.
- Sürüm kapısı: `product-images-no-test-key` (tek yazılım var, hiçbir cihaz yazılımında deneme anahtarı yok).

Flaş düzeni, kayıt biçimleri, telsiz sözcükleri değişmedi: bugünkü tasma ve gateway yükleme sonrası aynı rolde, kayıtları ve anahtarlarıyla açılır.

## Denendi

- `tools/ci/run_y2a_product_keys_host.sh` (ASan/UBSan): rol kararı (yazılı veri, boş cihazda seçim, çelişki, okunamayan bölge), deneme anahtarsız çalışma zamanı (gerçek anahtar + izin + doğrulayıcı; deneme anahtarlı tasmaya RAM izni olsa bile ret; deneme kurulum girişi nesne dosyasında yok).
- Eski host testleri: SF5L0, 3A, 3B (kaynak sözleşmesi tek yazılıma göre güncellendi), SF5L5, SF5L6, SF5L0 temizleme — PASS. `backend/tests/test_cihaz_yukle.py`, sürüm kapısı testi — PASS. Uygulama: 185 test (yeni `test/rolSecimi.test.ts`: öneri, rol yazma/ret, yeniden bağlanma ve izin reddi, seçilen rolde açılma doğrulaması), `tsc`, `vite build` — PASS.
- **ARM derleme denetimi** (bu ortamda PlatformIO derlemesi yok): sistem clang 18, `thumbv7em`/Cortex-M4, Adafruit nRF52 1.7.0 + SoftDevice S140 6.1.1 başlıkları, SX126x 2.0.32, SparkFun GNSS 2.2.29 — bütün `src/*.cpp` `rak4630_orun`, `rak4630` ve pilot bayraklarıyla hatasız; ARM nesne dosyalarında çift tanımlı sembol yok; tek yazılımın dışarıdan istediği her sembol eski tasma ya da gateway yazılımında da isteniyordu. Eski yazılımlar aynı denetimden geçti (denetimin doğruluğu). Bağlama ve gerçek boyut sahibin PC'sinde.

## Sahada sıra

Telefon: APK yeniden derlenmeli (`bash tools/orun_guncelle.sh`); rol sorusu yalnız boş cihazda görünür.


1. Tasma: `bash tools/orun_cihaz_yukle.sh` → tasmayı seç. `TAMAM` satırları; yazılım `0.5-<sürüm>`; açılışta `ROLE storage=TRACKER`.
2. Gateway: aynı komut → gateway'i seç (PC Edge yükleme boyunca durur). `ROLE storage=GATEWAY`; ardından tasmanın raporu uygulamada görünmeye devam etmeli.

## Sahada sonuç (2026-11-03)

Üç cihaz `0.5-416490d` yüklendi, hepsinde yükleme sonrası kontrol `TAMAM`: gateway (`09A4…5BA5`, held kayıtlar korundu, PC Edge yeniden bağlandı), tasma `0E8A…1AA3` (kimlik, kayıt sırası 3553'ten devam) ve tasma `52E1…8887` (kimlik ve kayıtlar korundu). Roller açılışta bellekten doğru anlaşıldı; hiçbir cihaza rol yeniden sorulmadı. PC Edge günlüğünde iki tasmanın kayıtları gateway üzerinden geldi (`custody_started` arttı, `denied=0`, `handed_off` arttı); uygulamada gateway "2 tasma (24 sa)".
