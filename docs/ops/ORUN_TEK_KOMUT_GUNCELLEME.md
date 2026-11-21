# ORUN — Mevcut tek komut güncelleme aracı

Giriş noktası: `tools/orun_guncelle.sh`. İkinci güncelleme sistemi kurulmaz.

## Normal kullanım

```bash
bash "$HOME/ORUN-TLP-SYSTEM/tools/orun_guncelle.sh"
```

Komut GitHub `origin/main` commit'ini getirir, **çalışan sunucu ve PC Edge'in gerçek sürümlerini ayrı ayrı** kontrol eder. Yalnız ilgili kaynak değiştiyse gereken bileşeni günceller. Değişiklik yoksa hizmetler yeniden başlatılmaz. Yerel `main` checkout dosyaları otomatik değiştirilmez; hedef kaynak GitHub'dan commit kimliğiyle sürümlenir.

```bash
bash "$HOME/ORUN-TLP-SYSTEM/tools/orun_guncelle.sh" --check
bash "$HOME/ORUN-TLP-SYSTEM/tools/orun_guncelle.sh" --server
bash "$HOME/ORUN-TLP-SYSTEM/tools/orun_guncelle.sh" --edge
bash "$HOME/ORUN-TLP-SYSTEM/tools/orun_guncelle.sh" --apk
```

- `--check`: yalnız bileşen planı; servis veya DB değişikliği yok (Git fetch ve güncelleme kilidi dosyası dışında).
- `--server`: yalnız Backend sunucusu. Önceki `--no-apk` artık sunucu-only alias'ıdır, **PC Edge'i restart etmez**.
- `--edge`: yalnız PC Edge.
- `--apk`: güncel **debug** APK'yı üretir (aynı sürüm varsa yeniden derlemez), **eski ORUN uygulaması indirme sayfasını** Tailscale IPv4 üzerinden varsayılan `8899` portunda açar. Telefonda `İndir ve kur` seçilir. Paylaşım `Ctrl+C` veya 15 dakika sonunda kapanır. USB/ADB kullanılmaz; otomatik uygulama kurulumu yoktur. `ORUN_APK_PORT` ile port değiştirilebilir. Otomatik akışta APK üretimi yok.
- `--all`: açıkça istenen sunucu + PC Edge + debug APK. Firmware flashing bu komutun kapsamında değildir.

## Güvenlik ve geri dönüş

1. Beklenen ORUN GitHub origin ve ileri doğru `main` tarihi kontrol edilir. Öngörülmeyen branch/kirli takipli dosya durumunda işlem durur.
2. Yeni backend/tools kaynakları SHA'lı sürüm klasöründe hazırlanır ve host regresyon testleri denenir.
3. Sunucu yükseltmesi öncesi çalışan sürüm, sağlık, kayıt sayısı ve kimliksiz erişim reddi doğrulanır. SQLite WAL uyumlu güvenli backup alınır, kimlik/hesap/defter bütünlüğü ile geri dönüş provası geçmeden kod değiştirilmez.
4. Eksikse `latest_location_utc` ve `latest_report_utc` indeksleri yalnız bir kez oluşturulur. Önce yedek kopyasında denenir, gerçek defterde tek transaction ile uygulanır; ACK, OBS1, cursor tabloları silinmez.
5. Sadece ilgili sunucu `systemd --user` ExecStart yolu değişir. Yeni süreçte gerçek komut yolu, HTTP sağlık, kayıt sayısının azalmaması ve hesap yetkisi sınanır. Sorunda eski servis tanımı geri yüklenir.
6. PC Edge kendi başlatma tanımı ve ayrı doğrulama/geri dönüşüyle güncellenir. Sunucu-only güncellemelerinde Edge'e dokunulmaz.
7. Yeni RAK firmware yükleme, Flash erase, APK kurma, kimlik anahtarını değiştirme, geçmiş/ACK kayıt silme **yoktur**.

**Sınır:** Rollback eski *kod ve servis tanımına* döndürür. Canlı DB'ye yeni veri geldiyse daha eski yedek canlı defter üzerine otomatik geri yazılmaz. Özellik bazlı büyük şema göçleri ayrıca açık migration gerektirir. Başarılı sağlık kontrolü uzun süreli saha kabulü yerine geçmez.

## İlk sefer / kaynak checkout güncellemesi

Kod `main`e alındıktan sonra yalnız bir kez, **yerel değişiklik yoksa**, checkout fast-forward edilmelidir:

```bash
git -C "$HOME/ORUN-TLP-SYSTEM" switch main
git -C "$HOME/ORUN-TLP-SYSTEM" pull --ff-only origin main
bash "$HOME/ORUN-TLP-SYSTEM/tools/orun_guncelle.sh" --check
```

Gelecek güncellemelerde yalnız normal tek komut yeterlidir. Test için:

```bash
python3 -m unittest -q backend.tests.test_orun_guncelle
```
