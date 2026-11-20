# Toplu onayın PC doğrulaması — 20 Kasım 2026

#356 ürün yazılımı adayı `0.5-78b49ec`. Sahadaki karşılaştırma sürümü `0.5-42a1104`. Tasma ve Gateway için aynı `rak4630_orun` imajı derlenir. Cihaz rolü kurulumdan, Gateway’in hangi tasmalara onay verebildiği imzalı izinlerinden gelir. Tasma kimliği derleme sırasında seçilmez.

## Hangi sonuç elde edildi?

- PC benzetiminde Gateway üç ayrı kaydı kalıcı belleğe yazıp geri okudu ve tek imzalı onay üretti. İlk onay kaybedildiğinde tasmanın yeni konumu Gateway'e ulaştı. İkinci onay komutun cevap penceresini bekledi. Tasma, onayda eksik bırakılan kaydı yeniden başlatmadan sonra da tuttu. Bu testte telsiz, canlı konum şifreleme donanımı ve komut cevabı benzetildi. Toplu onayın imzası ve doğrulaması gerçek koddan geçti.
- Yeni konum için tasma önce şifreli paketi kalıcı belleğe yazıyor. Gateway aynı paketi alıp kalıcı belleğinden doğrulayınca Edge için hazır ediyor ve toplu onaya dahil edebiliyor. Tasma bu ölçümü sırf geçmiş listesine girdi diye ikinci kez göndermiyor. Birleşik PC testinde ilk paket için verilen imzalı onay aynı kaydı kapattı.
- Onay bekleme süresi altı dakika. Bu süre dolunca tasma sıradaki onaysız kaydı aynı şifreli baytlarla yeniden gönderebiliyor. Yeniden başlatma bu bekleme süresini sıfırlıyor. Dört önbellek yeri dolunca Gateway toplu onayı göndermeye çalışıyor. Tasmanın beşinci yeri kritik olay için ayrılmış durumda. Dört yer doluyken yeni konum gelirse tasma konumu doğrudan gönderiyor ve kayıt bellekte kalıyor. Bu istisnada yer açılınca güvenilir geçmiş aktarımı için ikinci yayın gerekiyor.
- Doğrudan gönderilen canlı konumun başlığı eski okuyucularda reddediliyordu. Denemedeki Python, TypeScript ve Java okuyucuları bu konumu açıyor. Sunucu aynı ölçümün geçmiş kopyasını da aldığında tek mantıksal kayıt tutuyor. İmzası veya doğrulama etiketi geçersiz çerçeveler reddediliyor.
- Gateway her tasmanın onayını ayrı tutar. İki yetkili tasmadan aynı sayaçla gelen kayıtların ayrı onaylandığı, doğrudan canlı gönderimin kendi tasmasının eski onayına fırsat verdiği ve izinsiz tasmanın reddedildiği PC testinde doğrulandı. Tasma rolünde eski POSITION ve geçmiş tekrarlarının telsiz gönderimi kapalıdır.

## Telsiz süresi hesabı

`firmware/include/lora_airtime.h` hesabı ve `radio_config.h` ayarlarıyla (SF11, 125 kHz, 4/5) üç adet 100 baytlık ölçüm için tahmini toplam yayın süreleri aşağıda. Alım için açık kalma, kayıp, tekrar ve komut trafiği bu hesaba dahil edilmedi.

| Senaryo | Gönderilen çerçeveler | Toplam yayın süresi |
|---|---|---:|
| Eski sürümün tekli onayı | 3 ölçüm + 3 × 56 bayt onay | 10,84 sn |
| Yeni akışta tek onayda toplanan üç konum | 3 ölçüm + 1 × 72 bayt onay | 8,29 sn |
| Aynı üç ölçüm hem canlı hem geçmiş olarak yollanırsa | 6 ölçüm + 1 × 72 bayt onay | 14,94 sn |

8,29 sn hesabı Gateway üç ölçümü tek onaya alabildiğinde geçerli. Seyrek gelen ölçümlerde Gateway tek kayıt için de onay gönderebilir. Saha için karar verirken iki yöndeki yayınların tamamı, uyku/dinleme süresi, yeni konum gecikmesi ve kayıp kayıtlar birlikte ölçülmeli.

## PC'deki kontroller

`0.5-78b49ec` DFU paketinin SHA-256 özeti `bee06bff360796b3d015abc265829a134ed5747e6803220516cdfd6fb5a48ffe`. Derlenen imajın içinden sürüm okundu ve ZIP bütünlüğü doğrulandı.

| Komut | Sonuç |
|---|---|
| `bash tools/ci/run_kq05_tracker_selective_batch_release_host.sh` | Birleşik senaryo, değiştirilmiş onay, yetki iptali, kısmi yazma ve yeniden başlatma geçti |
| `python3 firmware/tests/sf5/test_sf5h_backend_verified_ledger.py` | Canlı kayıt ve geçmiş kopyası, anahtar ve içerik doğrulaması geçti |
| `python3 firmware/tests/sf5/test_sf5l8_backend_sync.py` (Node 22) | 130 kayıtta telefon ve sunucu harita verileri eşleşti |
| `bash tools/orun_android_mantik_test.sh` | 1.250 protokol vektörü, yerel defter, 12 köprü senaryosu ve sunucuya yükleme geçti |
| `cd edge/app && npm test && npm run build` (Node 22) | 321 test, TypeScript ve uygulama derlemesi geçti |
| `cd firmware && pio run -e rak4630_orun` | Tasma ve Gateway için tek ürün imajı derlendi |

Gerçek iki kartla onay kaybı, imzalı ayar cevabı, yayın süreleri ve enerji tüketimi henüz ölçülmedi. Sahaya geçişte tasma ve Gateway birlikte güncellenir. Önceki cihaz sürümüyle karışık kullanım hedeflenmiyor. Bu sonuçlar yalnızca taslağın PC aşamasını kapsıyor.
