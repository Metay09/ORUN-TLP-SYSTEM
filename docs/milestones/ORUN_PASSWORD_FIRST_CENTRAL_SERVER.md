# ORUN — tek hesap girişi, merkezi sunucu bağlantısı

**Durum: 31 Ekim 2026 — geliştirme kodu GitHub dalında, canlıya kurulmadı.**

## Ürün kararı
- Yeni telefon / fabrika sıfırlaması / yeniden kurulum: **yalnız kullanıcı adı ve parola**.
- P256, anahtar izi, QR, terminalden sahip onayı veya yeni telefon kaydı giriş için **zorunlu değil**.
- Kullanıcıya ikinci sunucu adresi ve teknik API anahtarı sorulmaz.
- Tailscale HTTPS geliştirme adresi: `https://simsek.tail823510.ts.net:8443`; gelecekteki kişisel alan adı tek `VITE_ORUN_ACCOUNT_ORIGIN` yapılandırmasıyla değişebilir.
- Gateway/Tracker provisioning ve PC Edge servis kimlikleri **kullanıcı hesabından ayrıdır**. Hesapla giriş yapmak cihaz firmware yetkisi vermez.

## Backend geliştirmesi
- Mevcut `owner_accounts.sqlite` üzerinde eklemeli `account_password_sessions` ve `account_password_attempts` tabloları.
- `POST /api/v1/account/login` bir telefon tokenı olmadan `{username,password}` kabul eder. Site ve actor veritabanından bulunur; istemci bunları veya izin kümesini seçemez.
- 12 saatlik yüksek entropili oturum tokenı, SHA-256 hash halinde sunucuda saklanır; kilitleme 5 başarısız denemeden sonra 15 dakika (tailnet pilot güvenlik sınırı).
- Tek kullanıcı adı birden fazla site'da varsa fail-closed; bilinmeyen site seçimi istemciye bırakılmaz.
- Yalnız `actor=owner` kullanıcıları owner izin kümesini alır. Yardımcı hesaplarına rastgele owner yetkisi verilmez.
- `GET /api/v1/account/me`, `GET /api/v1/sync` ve `POST /api/v1/sync/ops` hesap oturumuyla çalışır. Logout oturumu iptal eder. Eski onaylı telefon tokenları ve PC Ed25519 mirror hala uyumluluk için tutulur.
- Telefon istemez; buna karşılık Tailscale HTTPS erişimi ve parola gerekir. Henüz genel internette üretim güvenlik denetimi değildir.

## Gözlem kayıtları
- Yeni `backend/account_observation_relay.py` tek HTTPS kökeninin arkasından `GET /v1/edge/keys` ve `POST /v1/edge/batch` isteklerini **hesap oturumu doğrulandıktan sonra** eski gözlem Backend'ine iletebilir.
- Gözlem sunucusunun teknik bearer'ı yalnız **sunucu tarafında** 0600 gizli dosyadan okunur; Android'e aktarılmaz.
- İç relay hedefi yalnız **127/8 loopback veya Tailscale 100.64/10 IPv4** olabilir; URL ve token yalnız servis kurulum parametrelerindedir. Uygulama bunları seçemez.
- Relay kapalı veya yanlış ayarlanmışsa HTTP 503 döner; telefon kayıtları yerel SQLite'da bekler. **Başarılı gönderim iddiası yoktur**.
- Bağımsız `orun-backend.service` / `orun-pc-edge.service` süreçleri ve ayrı PC'deki edge kayıt akışı bu taslakta değiştirilmedi.
- Gerçek relay etkinleşmesi için sunucuya **bir defalık**, mevcut gözlem tokenını içeren özel dosya ve bilinen iç adresle yeni sürüm kurulumunun yapılması gerekiyor. Bu detaylar telefonun Ağ sekmesinde gösterilmez.

## Android
- `OwnerAccountClient.signInWithPassword` HTTPS üzerinden yalnız kullanıcı adı/parola gönderir. 12 saatlik token yalnız JS belleğinde; localStorage / SQLite'da saklanmaz.
- Hesap uygulama açılışında yeniden istenebilir; oturum yenileme ve çevrimdışı ilk giriş bu PR'de yoktur.
- Sunucu varsayılan Tailscale HTTPS adresi tek merkezde; `VITE_ORUN_ACCOUNT_ORIGIN` ile uygulama dağıtımında değiştirilebilir.
- Ayarlar ekranındaki zorunlu telefon kaydı kaldırıldı; **Hesabım** girişi kullanılır.
- Ağ ekranindeki sunucu adresi/teknik token formu kaldırıldı; mevcut Controller BLE/USB offline kayıtları hesap oturumu varken ortak HTTPS adresine gönderir.
- Önceden eski APK'da localStorage'a kaydedilmiş gözlem anahtarı yeni sürüm açılırken temizlenir. Telefonun kendi SQLite defteri ve Gateway kayıtları bu işlemle silinmez.
- Oturum yoksa yeni müşteri arayüzü otomatik upload başlatmaz. 401 veya ulaşamama durumu kalıcı kayıtları silmez.
- Kullanıcının girdiği parola yalnız login isteği sırasında kullanılır; kullanıcıdan yeni bir cihaz kaydı/onay kodu istenmez.
- "Hesabım" henüz uygulamanın tüm modüllerinin bağlandığı son ürün giriş kapısı değildir. Sürü/konum sunucudan çapraz cihaz otomatik pull da henüz tamamlanmadı.

## Kabul ve dağıtım
- Tam otomatik test komutu: `bash tools/ci/run_account_password_only_host.sh`.
- **Bu dalda host/Android fiziksel PASS henüz alınmadı**, GitHub Actions aktif olmayabilir.
- Güncel çalışır S6O account Backend (localhost 18781) ve kullanıcıdaki mevcut APK bu PR ile değiştirilmedi. Canlı PC Edge ve Tailscale Serve aynı durumda.
- S6I eski özel staged kaynak dosyaları bu PR'nin dosyalarını içermez. Üretime almadan **yeni sürümlü runtime paketi**, kontrollü systemd unit güncellemesi ve 8443 üzerinden gerçek HTTP/Android kontrolü gerekir.
- Kullanıcının kararı: Küçük düzeltmeler için APK üretme; geliştirme sonunda **tek genel uçtan uca test ve bir APK güncellemesi**. Test verisi yedeği öncelik değil; çalışan gözlem servisi ve PC Edge erişilebilir kalacak.
- Gerekli sonuç: Telefonda/yeniden kurulmuş ikinci telefonda P256 anahtarı/terminal onayı olmaksızın giriş, kaynaklara erişim ve hesap yetkisiyle gözlem yükleme; aynı PC ya da başka PC'nin bağımsız Gateway aktarımı korunmalı.
