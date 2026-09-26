# Gateway komut yetkisi mimarisi — keşif raporu

Status: **EXPLORATION ONLY — NOT OWNER-APPROVED.** Bu belge bir güvenlik + ürün +
saha mimarisi keşif çalışmasıdır. Hiçbir wire formatı, SecurityStore formatı,
runtime davranışı veya ADR değişikliği onaylamaz. Mevcut kurallar
(`AGENTS.md`, `ORUN_CURRENT_ARCHITECTURE_RULES.md`,
`ADR_M7P6_SECURITY_ARCHITECTURE.md`) geçerliliğini korur.

**Owner follow-up (2026-09-25):** Bu keşif raporunun `H = A + D0` önerisindeki
"offline gateway-originated command yalnız canlı RX penceresinde" fedakârlığı
**owner tarafından kabul edilmedi**. Güncel owner-approved ürün/mimari yönü
`docs/architecture/ORUN_GATEWAY_COMMAND_AUTHORITY_DIRECTION.md` kaydındadır.
Özellikle offline, önceden enrolled gateway komutu opaque relay üzerinde
store-forward edilip sleepy tracker uyanana kadar bekleyebilmelidir. Kabul edilen
availability fedakârlığı, ilk/yeni/re-enrollment sırasında Internet/backend
gerekebilmesidir.

Analiz tabanı: `feat/m7p6f-securitystore-v2-replay-state@3e7dc2d` (2026-09-24).
Okunan kaynaklar: AGENTS.md, architecture/README, CURRENT_ARCHITECTURE_RULES §15–17,
ADR_M7P6, M7P6D, M7P6F, DOWNLINK_RENDEZVOUS_PLAN, FIELD_NETWORK_DIAGNOSTICS §11,
APP_ENTITY_MESSAGING §4, `security_store.h`, pinlenmiş CC310 başlıkları.
Birincil kaynaktan doğrulanan: RFC 9203, RFC 8613 Ek B, RFC 9175. Matter, Z-Wave S2,
Thread ve LoRaWAN 1.1 karşılaştırmaları bilgiye dayanır, bu çalışmada kaynaktan
kontrol edilmedi. Airtime değerleri formülle hesaplandı, sahada ölçülmedi.
Hiçbir sonuç fiziksel donanım kanıtı içermez.

---

## 1. Kısa sonuç

1. **Aday B (ADD_GATEWAY dağıtımı) birincil mimari olarak reddedilmeli.** Her yeni
   gateway veya revoke işlemi, filo büyüklüğüyle orantılı RF trafiği ve flash yazımı
   gerektiriyor. Uzun süre görünmeyen tracker'lar yüzünden yetki durumu kalıcı olarak
   parçalı kalıyor. Tracker'da gateway başına dayanıklı replay durumu tutmak gerekiyor.
2. **Aday A doğru ama yetersiz.** Backend kaynaklı ve store-forward edilebilen komutlar
   için doğru yol. Olduğu gibi kalmalı ve M7P6F'yi kullanmalı. İnternet yokken yerel
   komut ihtiyacını çözmüyor.
3. **Sertifika fikri (Aday C) ciddi ve standartlarda karşılığı var** (Matter NOC/CASE,
   EDHOC). Ancak SF11 LoRa, uyuyan tracker ve az sayıda gateway için gereğinden pahalı.
   En büyük sorun: tracker'ın RESULT'unu çevrimdışı gateway'in doğrulayabilmesi için
   tracker'da da asimetrik anahtar ve tam el sıkışma gerekiyor. Bu RF maliyetini 3–4
   katına çıkarıyor ve relay üzerinden canlı dönüş penceresine sığmıyor.
4. **Öneri: Hibrit H = A + D.**
   - **Backend yolu (A):** store-forward yapılabilir, M7P6F'yi olduğu gibi kullanır.
   - **Türetilmiş delegasyon yolu (D):** Backend, tracker'ın zaten paylaştığı sırdan her
     (tracker, gateway, grant) üçlüsü için tek yönlü bir delegasyon anahtarı türetir ve
     gateway'e verir. Capability'ler ve geçerlilik sınırı KDF girdisine gömülüdür.
     Tracker anahtarı her komutta kendi `K_root`'undan yeniden hesaplar.
   - Tracker'da **gateway başına kalıcı durum yok**, yeni gateway için filoya **sıfır RF**,
     LoRa çerçevesinde **imza yok**.
   - Tazelik, tracker'ın kimliği doğrulanmış uplink'inden gelen rendezvous challenge ile
     oturum anahtarına bağlanır. Replay, pre-play ve gateway sayaç rollback'inden
     kaynaklanan nonce tekrarı yapısal olarak engellenir.
   - Geçerlilik süresi saatle değil, **tracker'ın kendi monoton D2A sayacıyla** ölçülür.
5. **Kabul edilen fedakârlık:** Çevrimdışı gateway kaynaklı komutlar yalnız **doğrudan
   veya relay üzerinden canlı (aynı RX penceresi içinde)** teslim edilir. Dakikalar/saatler
   süren store-forward yalnız backend kaynaklı komutlar için geçerlidir.
6. **Uyarı:** H, ADR'deki "gateway güvenlik otoritesi değildir" kuralını açıkça değiştirir:
   yeni sınıf "backend tarafından delege edilmiş, kapsamlı gateway" olur. ADR §2 bunu
   öngörüyor ("explicit site-local trusted endpoint may be designed separately"), yine de
   sahip onayı gerekir.
7. "İnternetsiz yerel konum görüntüleme" gereksinimi ile "gateway D2A'yı çözemez" kuralı
   çelişiyor. Bu, ayrı bir okuma delegasyonu kararı gerektirir (§10).

## 2. Gereksinimlerden çıkan güvenlik modeli

- **Güven kökü backend.** Tracker ve backend tracker başına simetrik `K_root` paylaşıyor;
  backend D2A doğrulayıcı ve A2D göndericisi (M7P6D). Backend tracker sırrına zaten sahip.
  Tracker'a asimetrik trust anchor eklemek, var olan simetrik kökün yanına ikinci kök
  eklemek demektir.
- **Yetki devri:** backend → gateway; kapsamlı, sınırlı süreli, iptal edilebilir.
- **Tracker'da saat yok.** Tek güvenilir monoton ve dayanıklı "saat" D2A TX rezervasyon
  sınırı (M7P6B): reboot'ta ileri atlar, asla geri gitmez.
- **Tazelik kaynağı tracker'ın kendisi:** uyanır, kimliği doğrulanmış uplink gönderir,
  pencere açar. RFC 9175 Echo'nun olay tabanlı tazeliğinin asenkron karşılığı.
- **Çevrimdışı gateway iki yönde doğrulama yapabilmeli:** "Başarılı" gösterebilmek için
  tracker RESULT'unu da doğrulamalı. Bu, tracker ile gateway arasında paylaşılan anahtar
  gerektirir.
- **Nonce disiplini:** M7P6D'deki her güvenlik bağlamı için tek gönderen kuralı korunur.
- **Tehdit gerçekçiliği:** nRF52840 APPROTECT'in glitching ile aşılabildiği kamuya açık.
  Çalınan tracker/gateway için anahtarlar çıkarılmış varsayılmalı; etki alanı cihaz ve
  kapsam başına sınırlı olmalı.

## 3. Aday A: Her komutun yetkilisi backend

| Konu | Değerlendirme |
|---|---|
| Güçlü | Gateway opak. Relay/store-forward doğal. M7P6F katı HWM uygun. Tracker'da gateway durumu yok. ADR ile uyumlu. |
| Zayıf | İnternet yoksa yeni komut yok. Seyyar gateway tek başına işe yaramaz. |
| Gizli sorun | Geç teslim edilen çerçeve replay değildir (M7P6D §5.3). Komut düzeyinde son kullanma sınırı gerekir; saat olmadığından tracker D2A sayacına göre: "tracker sayacı < C_exp iken geçerli". |
| Karar | Kalmalı; birincil çevrimiçi/store-forward yolu. Tek başına yetersiz. |

## 4. Aday B: ADD_GATEWAY

- **Maliyet:** Yeni gateway başına 1000 × ~70 B A2D ≈ 1,64 s SF11 airtime. Retry ile
  ≈ 2100 s, RESULT'larla ≈ 3000 s. Tracker yalnız uyandığında ulaşılabildiği için günler.
- **Parçalı durum:** 830/170 kalıcılaşır; GW-C 170 tracker'a komut veremez. Revoke da
  aynı maliyetle ikinci dağıtım.
- **Tracker durumu:** gateway başına kayıt + dayanıklı replay HWM; M7P6F gateway başına
  genişletilmeli; slot tahliye politikası gerekir (tahliye edilen HWM → eski çerçeveler
  yeniden kabul edilebilir).
- **Ek yüzey:** sahte/replay ADD_GATEWAY, slot DoS, güven güncellemesinde güç kesintisi.
- **Karar:** Reddedilmeli. D aynı işi sıfır RF ve sıfır tracker yazımıyla yapar. Kurtarılacak
  tek fikir: ileride çevrimdışı store-forward için önbellek niteliğinde HWM slotları (§11).

## 5. Aday C: Tracker yalnız backend trust anchor bilir, gateway sertifika sunar

Tracker'da backend public key'i (P-256/Ed25519); gateway kendi anahtar çiftini üretir,
backend `{gw_pub, site, caps, grant_gen, epoch}` sertifikası imzalar.

**C1 — tek atışlık imzalı komut:** gateway `(tracker challenge ‖ komut)` imzalar + sertifika.
- İlk temas ≈ 200 B (≈ 4,0 s), RAM önbellekli ≈ 100 B (≈ 2,2 s).
- Gizlilik yok (geofence poligonu açık gider).
- Tracker RESULT'u çevrimdışı gateway tarafından doğrulanamaz.

**C2 — EDHOC/CASE benzeri AKE:** tracker'da da backend imzalı anahtar çifti + sertifika.
- 3 mesaj; sertifika değer olarak taşınırsa mesaj 2 ≈ 150–200 B; toplam ≈ 6–8 s, tracker ek TX.
- Oturum RAM'de ise her uyanışta tekrar; dayanıklı ise gateway başına durum geri gelir.

| Soru | Yanıt |
|---|---|
| Saat yokken expiry | Tek sertifika tüm tracker'lar için geçerli olduğundan tracker başına sayaç sınırı konamaz. Global epoch ratchet ya da "son backend kanıtından beri sayacım N'den fazla ilerlediyse reddet" tasması; ikisi de tracker'da dayanıklı epoch ister. |
| Offline revocation | Yalnız epoch ratchet; hiç temas yoksa mümkün değil. |
| Çalıntı gateway süresi | Ratchet veya tasma tetiklenene kadar. |
| Her komutta sertifika? | Hayır (RAM önbelleği), ama 64 B imza her komutta. |
| Oturum önbelleği | Yalnız C2 ile. |
| Tracker reset | Önbellek gider, ilk temas pahalılaşır; güvenlik sorunu yok. |
| Gateway reset | Yeni anahtar çifti + sertifika; eski sertifika yalnız epoch ile ölür. |
| 4–8 slottan iyi mi? | B'den iyi. D'den kötü: 3–4× RF, ECC koeksistansı kanıtsız (pinli CC310'da `crys_ecpki_ecdsa.h`, `crys_ec_edw_api.h` var ama ORUN'da/Bluefruit ile test edilmedi), sertifika parser'ı, canlı relay penceresine sığmaz. |

**C'nin gerçek avantajı:** gateway tracker'a özel malzeme tutmaz; gateway'in son
senkronizasyonundan sonra kaydedilen tracker'larla da çevrimdışı çalışır. Çok kiracılı
altyapıda veya backend'in tracker sırrını bilmediği bir modelde C üstündür.

## 6. Aday D: Backend türetimli tracker × gateway delegasyonu + rendezvous'a bağlı oturum

Kerberos servis bileti + anahtar çeşitlendirme + ACE-OSCORE birleşimi. RFC 9203'te AS
OSCORE master secret'ı üretir, token içinde RS'e şifreli iletir, bağlam N1/N2'den türetilir.
D, token'ı taşımak yerine türetir; havada token blob'u yoktur.

```text
Backend (online, enrollment/refresh):
  K_TG = HKDF(K_deleg_T, info = label_DELEG ‖ gw_id ‖ grant_gen ‖ caps ‖ d2a_expiry)
  (K_deleg_T = HKDF(K_root_T, "deleg") veya K_root; custody kararı ayrı)
  → GW'ye site tracker'larının {T, K_TG, caps, d2a_expiry} paketi (gateway cihaz anahtarına sarılı)

Saha (çevrimdışı olabilir):
  Tracker v2 D2A uplink → RAM rendezvous = H(d2a_counter ‖ tag ‖ header)
  GW  : K_sess = HKDF(K_TG, salt = rendezvous, label_SESSION)
        CMD  = AEAD_{K_sess, nonce = gw_session_ctr}(AAD: T, gw_id, grant_gen, caps, d2a_expiry, command_id)
  Tracker: K_TG'yi kendi K_root'undan hesaplar (durum yok)
         → caps / expiry (kendi D2A sınırı < d2a_expiry) / pencere eşleşmesi → AEAD
         → yürüt → RESULT = AEAD_{K_sess, T2G}
  GW  : RESULT'u çevrimdışı doğrular → "Başarılı"
```

- **Tracker kalıcı durumu:** gateway başına sıfır ("D0"). Hızlı revoke için "D1": tek
  dayanıklı `delegation_epoch` (§14).
- **Capability yükseltme imkânsız:** caps KDF girdisinde.
- **Nonce güvenliği:** `K_sess` her tracker uplink'i için benzersiz (D2A sayacı credential
  başına tekrar etmez). Gateway oturum sayacı yalnız RAM'de. Gateway reset/restore/klon
  doğrudan modda nonce tekrarı üretmez (RFC 8613 Ek B.2 deseni). A2D nonce'u rendezvous'tan
  türetilmez; türetilen anahtardır.
- **Backend denetimi:** backend `K_TG` ve `K_sess`'i türetebilir; çevrimdışı gateway
  komutlarını sonradan doğrulayabilir.
- **Bedel:** gateway tracker başına ≈ 32 B (1000 → ≈ 32 KB; RAK4631 gateway'de yeni flash
  sahipliği kararı). Gateway senkronizasyonundan sonra kaydedilen tracker'lar internet
  gelene kadar o gateway'den komut alamaz. Gateway ele geçerse sitedeki tüm tracker'lara
  kapsam/süre içinde komut verilebilir (B ve C'de de aynı).

## 7. Hibrit H = A + D

| Durum | Yol |
|---|---|
| Çevrimiçi, tracker uyuyor | A: backend çerçeve üretir, gateway/relay opak saklar, uyanınca teslim. M7P6F HWM + D2A sayacına bağlı expiry. |
| Çevrimdışı veya düşük gecikme | D: gateway kendi delegasyonuyla bir sonraki uplink penceresinde. |
| Çevrimiçi kritik komut | A (backend kullanıcı yetkisi, MFA, denetim). |

Reddedilen alternatifler:

- **Telefon yetki taşır:** gateway opak kalır ama telefon tracker uyanana kadar bağlı
  kalmalı; "telefon yalnız arayüz" tercihiyle çelişir.
- **Önceden üretilmiş komut kuponları:** katı HWM sırayı kilitler; D kapsar.
- **Site/filo ortak anahtarı:** tek tracker'dan çıkarım tüm siteyi açar; çoklu göndericide
  nonce koordinasyonu imkânsız; ADR §3.2 ile çelişir. Kesin ret.

## 8. Karşılaştırma tablosu

| | A | B | C (C1/C2) | D | **H = A + D** |
|---|---|---|---|---|---|
| Güven kökü | Backend | Backend → tracker listesi | Backend public key | Backend (simetrik, mevcut) | Backend |
| Komutu üreten | Backend | Gateway | Gateway | Gateway (delege) | Backend / gateway |
| İnternetsiz | Hayır | Evet | Evet | Evet | Evet |
| Store-forward | Evet | Dayanıklı HWM ile | C1 kısmen | Yalnız canlı pencere | A evet, D canlı |
| Tracker kalıcı durum | M7P6F | Slot × (anahtar + HWM) | Trust anchor + epoch | 0 (D1: 1 epoch) | M7P6F (+ ileride 1 epoch) |
| Gateway kalıcı durum | Kuyruk | Anahtar + dayanıklı sayaç | Anahtar çifti + sertifika | ≈ 32 B/tracker | Aynı |
| Yeni gateway UX | — | Günler, kısmi | Anında | Anında (senkronize tracker'lar) | Anında |
| Revoke | Gerekmez | Filo RF'si | Epoch/tasma | Sayaç expiry (+ epoch) | Aynı |
| Komut başına RF | ≈ 40–50 B | ≈ 45 B | 100–200 B / el sıkışma | ≈ 45–55 B | ≈ 45–55 B |
| Canlı relay penceresi | Gerekmez | Sığar | Sığmaz | Sığar | Sığar |
| Asimetrik kripto | Yok | Yok | Var | Yok | Yok |
| Karmaşıklık | Düşük | Yüksek | Yüksek | Orta | Orta+ |

## 9. İnternet VAR

Kullanıcı "Gönder" → backend kullanıcıyı yetkilendirir → A çerçevesi → tracker'ı duyan
gateway(ler)e dağıtılır → bir sonraki uplink'te teslim → kimliği doğrulanmış RESULT →
"Başarılı". Kullanıcı: "Sırada (≈ ≤ 30 dk) → Teslim edildi → Uygulandı ✓". Yavaş internette
canlı backend round-trip'i beklenmez; A kuyruğa dayalı.

## 10. İnternet YOK

Telefon BLE ile gateway'e bağlanır, PIN girilir. Gateway grant paketinde tracker + capability
var mı bakar; varsa komut sıraya alınır, tracker uplink'inde D ile iletilir, RESULT gateway'de
doğrulanır. Kullanıcı: "Yerel mod — GW-C bu hayvana komut verebilir" ya da "yerel yetki
yok / süresi doldu — internet gerekli".

**Açık sorun:** çevrimdışı konum görüntüleme için D2A'nın gateway'de çözülmesi gerekir.
Seçenekler: GET_POSITION'ı D oturumuyla istemek veya tracker başına ayrı site okuma
anahtarı. Komut mimarisinden bağımsız karar.

## 11. Relay / store-forward

**A:** çerçeve kendi başına geçerli (sayaç + expiry) ve opak; relay tracker sırrı olmadan
saklar/iletir; birden çok kopyada HWM ilkini kabul eder. TLP v1 tek hop'ta donmuş; çok hop'lu
store-forward ayrı ağ milestone'u gerektirir.

**D:**
- Canlı relay (TRACKER → R1 → GW → R1 → TRACKER, aynı 10 s pencere) çalışır, relay opak.
  Bütçe: uplink en geç 5,43 s + 2 × ~1,23 s + 0,4 s ≈ 8,3 s; ~1,7 s pay.
- 20 dk bekleyen store-forward çalışmaz; bilinçli fedakârlık.
- İleride gerekirse: gateway'de dayanıklı rezervasyonlu sayaç + tracker'da 4–8 önbellek HWM
  slotu; slot yalnız canlı oturumda "sayacım şu an N" beyanıyla kurulur. Slot kaybı yalnız bir
  canlı temas gerektirir. Format değişikliği; talep olmadan yapılmamalı.

**Rendezvous doğru araç:** D ve C'nin canlı oturumları. **Yanlış araç:** A'nın kuyruğa alınmış
çerçeveleri ve store-forward (orada sayaç + HWM + sayaca bağlı expiry).

## 12. Yeni gateway enrollment

| Seçenek | Sonuç |
|---|---|
| 1. GW-A tracker'lara ADD_GATEWAY dağıtır | Ret; GW-A'yı yetki dağıtıcısı yapar + B maliyetleri. |
| 2. Backend ADD_GATEWAY dağıtır | Ret; O(filo) RF, 170 ulaşılamaz. |
| 3. GW-C sertifika gösterir | Çalışır, pahalı (C). |
| 4. Tracker yalnız trust anchor doğrular | C ile aynı. |
| **5. Türetilmiş delegasyon (D)** | **Önerilen.** |

Akış: uygulamada "Yeni Gateway" → etiket/QR sahiplenme sırrı + gateway'in ürettiği anahtar
çifti backend'e bağlanır → capability'ler atanır → grant paketi → sahaya götür → çalışır.
830/170: GW-C bin tracker'ın paketini baştan alır; 170'i duyulduğu anda komut verilebilir
(d2a_expiry aşılmamışsa; uzun görünmeyenler için geniş tutulabilir).

## 13. Factory reset

**Gateway:** grant paketi, cihaz anahtar çifti ve PIN silinir; fabrika PIN'ine dönülür. Yeniden
kayıt kesinlikle yeni `grant_gen` alır; backend bir nesli asla yeniden vermez. "Aynı anahtar +
sayaç=0" yapısal olarak oluşamaz. Yedekten geri yükleme özellik olarak olmamalı; adli restore
"klonlanmış gateway" tehdididir: nonce tekrarı yok, yalnız süresi dolmamış eski yetki kalır.

**Tüm gateway'ler reset + internet yok:** yerel komut çalışmaz; internet beklenir (kabul edilmiş).

## 14. Gateway çalınması / revoke

| Yöntem | Değerlendirme |
|---|---|
| Tracker listesinden silme | Yalnız B'de; pahalı. |
| **D2A sayacına bağlı expiry (D0)** | Saatsiz. Çalıntı gateway tracker sayacı `d2a_expiry`'yi geçene kadar yetkili; backend yenilemez. |
| **Epoch ratchet (D1)** | Tracker `delegation_epoch` saklar; güncel gateway/backend'den gelen kimliği doğrulanmış daha yüksek epoch ratchet eder; iptal ilk temasla yayılır. |
| Saate bağlı kısa ömürlü token | Ret; güvenilir saat yok. |
| Backend imzalı revocation listesi | Sınırsız liste; ret. |

**Kalan risk:** aylarca çevrimdışı tracker, sayacı expiry'yi geçene kadar çalıntı gateway'in
kapsam içi komutlarını kabul eder; D1 ile güncel gateway'le ilk temasa kadar kısalır.
Kaçınılmaz, açıkça yazılmalı.

**Kalibrasyon:** D2A sayacı gürültülü saat; reboot başına 256 atlar, expiry'yi erken getirir
(güvenli yön, kullanılabilirlik etkisi).

**Blast radius:** gateway capability'leri credential reset, DFU ve muhtemelen OPEN_BLE içermemeli.

## 15. Tracker reset / power-cut

- **Reboot:** RAM rendezvous/oturum kaybolur (güvenli); D2A sayacı ileri atlar.
- **Uygulandı ama RESULT gitmeden reset (ABC123):**
  - Salt okuma: tekrar yürütülebilir.
  - Durum ayarlayan (config, geofence): versiyonlu CAS ile idempotent; ConfigStore versiyonu
    dayanıklı kanıt; tekrar gelirse "zaten uygulandı".
  - Tek atışlık/kritik eylem: kaynak veya eylem sınıfı başına dayanıklı "son op_id"; durum
    kaynak sayısıyla sınırlı, sınırsız command_id geçmişi yok.
- **Flash arızası:** SecurityStore FAULT, korumalı işlemler kapalı; "Güvenlik hatası — servis".
- **Tracker güvenlik reset'i:** yalnız fiziksel servis; yeni `K_root` eski delegasyonları
  otomatik geçersiz kılar.

## 16. Katman ayrımı

| Katman | A yolu | D yolu |
|---|---|---|
| Authorization | Backend kullanıcı ACL'i | Backend ACL → grant caps (KDF'de) + gateway'de PIN'li operatör |
| Authentication | K_root A2D anahtarıyla AEAD | K_sess (← K_TG ← K_root) ile AEAD |
| Freshness / replay | M7P6F katı HWM + D2A sayacına bağlı expiry | Rendezvous challenge (tek pencere, RAM) |
| Nonce safety | Backend reserve-ahead sayacı (M7P6D) | Oturum başına benzersiz anahtar + RAM sayacı |
| Idempotency | command_id + versiyon/CAS | Aynı |
| Delivery | TX_DONE ≠ teslim | Aynı |
| Result | Kimliği doğrulanmış RESULT → backend | Kimliği doğrulanmış RESULT → gateway (çevrimdışı) |

## 17. PIN / yerel erişim

- **Fabrika PIN:** cihaz başına benzersiz, etiket/QR üzerinde. Tüm cihazlarda aynı PIN reddedilir.
- **Neden kritik değil:** factory reset'li gateway hiçbir yetki taşımaz; fabrika PIN'i boş cihazı korur.
- **Dikkat:** PIN kriptografik yetki değil, ama PIN + gateway erişimi = o gateway'in yetki kapsamı.
- **Önlemler:** PIN yalnız şifreli kanaldan (LESC), gateway'de salt'lı hash; deneme sayacı
  dayanıklı ve doğrulamadan önce artırılır; üstel backoff; son çare fiziksel düğmeyle factory
  reset (grant'i siler); PIN'den anahtar türetilmez; yerel komutlar denetim kaydına yazılıp
  backend'e senkronize edilir.

## 18. Saldırı tablosu (H için)

| # | Saldırı | Mümkün mü / etki | Engelleyen | Kalan risk |
|---|---|---|---|---|
| 1 | Pasif RF dinleme | Metadata görülür | AEAD | Trafik analizi, tracker ID açık |
| 2 | Kayıtlı paket replay | Hayır | A: HWM; D: rendezvous | — |
| 3 | Pre-play | Hayır | D: challenge uplink tag'ine bağlı; A: HWM | — |
| 4 | Sahte tracker uplink | Challenge üretemez | D2A AEAD | DoS |
| 5 | Sahte gateway | Komut üretemez | K_TG yok | Jamming |
| 6 | Çalıntı gateway | Evet, kapsam/süre içinde | Caps, expiry, D1 epoch | Offline tracker'larda süre dolana kadar |
| 7 | Factory reset'li çalıntı gateway | Grant yok; önceden çıkarıldıysa #6 | Silme + yeni nesil | #6 |
| 8 | Gateway depolama klonu | #6; nonce tekrarı yok | Oturuma bağlı K_sess | #6 |
| 9 | Eski yedekten restore | #6 | Aynı | #6 |
| 10 | Çalıntı telefon | Tracker yetkisi yok | Backend oturum iptali, PIN önbelleğe alınmaz | Açık oturumda #12 |
| 11 | PIN kaba kuvvet | Yavaşlatılmış | Dayanıklı backoff | Zayıf PIN |
| 12 | Ele geçirilmiş backend hesabı | A ile kapsamındaki komutlar | MFA, kritik komut politikası | Kullanıcı kapsamı |
| 13 | Kötü niyetli relay | Düşürür/geciktirir | AEAD; D penceresi | DoS |
| 14 | Ele geçirilmiş yetkili gateway | #6 | Backend denetimi | #6 |
| 15 | İki bağlantısız gateway yarışı | Çakışan komutlar | CAS/versiyon, pencere başına tek oturum | "Çakışma" RESULT'u |
| 16 | Revoke edilmiş gateway + offline tracker | Evet | Expiry, D1 | Kabul edilen kalan risk |
| 17–18 | Sahte/replay ADD_GATEWAY | Mesaj yok | — | — |
| 19 | Eski sertifika replay | Sertifika yok; eski grant = #16 | Expiry | #16 |
| 20 | Tracker'lar arası replay | Hayır | K_TG tracker'a özgü | — |
| 21 | Gateway'ler arası replay | Hayır | gw_id KDF'de | — |
| 22 | Modlar arası replay | Hayır | Ayrı KDF etiketleri (A2D/DELEG/SESSION/T2G) | Etiket kaydı disiplini |
| 23 | Sayaç rollback | A: M7P6D kuralı; D: etkisiz | Oturum anahtarı | Backend DB restore kuralı |
| 24 | Güven güncellemesinde güç kesintisi | Yalnız D1 epoch; commit-last | SecurityStore | FAULT, erişilebilirlik |
| 25 | Flash bozulması | Fail-closed | SecurityStore | Servis |
| 26 | Slot doldurma DoS | Slot yok | — | — |
| 27 | Flash aşındırma | Kimliği doğrulanmamış girdi yazamaz | M7P6F | Yetkili sayaç sıçraması (M7P6F L4) |
| 28 | Jamming | Evet | Yok | Fiziksel |
| 29 | Credential downgrade | Hayır | Caps KDF'de, etiket versiyonu | — |
| 30 | Eski firmware downgrade | D1 bilmeyen firmware'e dönülebilir | Bootloader anti-rollback BİLİNMİYOR (ADR §12) | Açık risk |

## 19. Arıza / kullanılabilirlik: kullanıcı ne görür

| Durum | Kullanıcının göreceği |
|---|---|
| İnternet var | "Sırada → Uygulandı ✓" |
| İnternet yok | "Yerel mod"; gateway'de yetkili hayvanlara komut |
| Backend erişilemiyor | İnternet yok ile aynı; bulut kuyruğu bekler |
| Base gateway yok | Seyyar gateway ile yerel komut |
| Yalnız seyyar gateway | Menzildekiler "komut verilebilir", diğerleri "menzil dışı" |
| Gateway tracker'ı doğrudan görüyor | Bir sonraki uplink'te |
| Arada yalnız relay | A: normal. D: canlı pencereye sığarsa; aksi halde "internet gerekli" |
| Gateway 6 ay kapalı | "Yerel yetkinin süresi doldu (N hayvan) — senkronizasyon gerekli" |
| Tracker 6 ay kapalı | "Uzun süredir görülmedi"; grant muhtemelen geçerli |
| Yeni gateway | Bir kez commissioning, hazır |
| Yeni tracker | Gateway senkronize olana kadar "bu gateway'de yetki yok" |
| Bir gateway factory reset | "Yeniden kurulum gerekli (internet)" |
| Tüm gateway'ler reset | "Yerel komut kullanılamaz — internet gerekli" |
| Gateway çalındı | "İptal et" → backend yenilemeyi keser; "aylarca görülmeyen hayvanlar için risk sürüyor" |
| Revoke + tracker aylarca offline | Uyarı, kalan risk gösterilir |
| Telefon değişti | Giriş yapılır; anahtar taşınmaz |
| İki telefon | Sorun yok |
| İki bağlantısız gateway | Olası "çakışma" sonucu |
| Tracker reset | Görünmez |
| Tracker flash arızası | "Güvenlik hatası — servis" |
| Gateway flash arızası | "Gateway yeniden kurulmalı" |
| Gateway saati yanlış | Etkisiz |
| Tracker'da GNSS zamanı yok | Etkisiz |
| İnternet çok yavaş | A kuyruğu gecikir; yerel D kullanılabilir |
| RX penceresi kaçtı | "Sonraki raporda tekrar denenecek" |
| Relay saatler sonra getirdi | A: expiry içindeyse uygulanır, değilse "süresi doldu"; D: red |
| Acil komut, tracker uyuyor | "Bir sonraki uyanış ≈ X dk"; ileride LOST/SEARCH aralıkları kısaltabilir |

## 20. RF / güç / depolama

SF11/BW125/CR4/5 + LDRO; payload ≈ 18,2 ms/B (34 B ≈ 987 ms, 49 B ≈ 1233 ms; repo ile tutarlı).

| | D komutu ≈ 48 B | D RESULT ≈ 36 B | A ≈ 45 B | B ADD ≈ 70 B | C1 ilk ≈ 200 B | C1 önbellekli ≈ 100 B |
|---|---|---|---|---|---|---|
| Airtime | ≈ 1,23 s | ≈ 0,99 s | ≈ 1,2 s | ≈ 1,64 s | ≈ 4,0 s | ≈ 2,2 s |

- **Tracker (H):** ek RAM ≈ 64–96 B; flash 0 B (D1'de tek epoch kaydı); 2–3 HKDF + 2 CCM
  (ms mertebesi); RX penceresi değişmez (10 s).
- **Tracker (C):** ECC doğrulama (CC310 süresi ölçülmedi), birkaç KB stack, yüzlerce B önbellek.
- **Gateway (D):** ≈ 32 B × tracker; doğrudan modda dayanıklı sayaç gerekmez.
- **Enrollment RF:** D/C sıfır; B gateway başına ≈ 2–3 bin s.
- **Duty cycle:** 869.4–869.65 MHz tipik olarak %10 (bölgesel kuralla teyit edilmeli, sabitlenmemeli).
  B dağıtımı ≥ 6 saat saf airtime, uyanma süreleriyle pratikte günler.

## 21. Ölçek

| Tracker | B | C | D / H |
|---|---|---|---|
| 10 | Yönetilebilir | Çalışır | Çalışır |
| 100 | Gateway başına ~5 dk airtime, parçalı | Çalışır | Gateway'de ≈ 3,2 KB |
| 1000 | ~45 dk airtime, günler, 170 ulaşılamaz | Çalışır, komut RF'si pahalı | Gateway'de ≈ 32 KB, RF sıfır |

## 22. Karışık filo

TLP v1 dokunulmaz. Eski firmware yeni D etiketini tanımaz, düşürür (fail-closed); uygulama
"firmware güncellemesi gerekli" gösterir. 2026-09-23 notuna göre müşteri filosu yok; v2
geçişinde tüm geliştirme donanımı birlikte güncellenebilir, kalıcı dual-stack gerekmez.

## 23. M7P6F etkisi

- **Olduğu gibi kullanılır:** A yolu M7P6F'nin tek backend göndericisi + katı HWM modeline dayanır.
- **D yolu M7P6F'ye dokunmaz:** replay durumu oturuma bağlı, RAM'de.
- **SecurityStore v2 formatı şimdi değişmemeli.** D1 epoch veya store-forward HWM slotları
  yeni `kind` gerektirir; M7P6F'de bilinmeyen kind FAULT olduğundan açık format revizyonu
  gerekir. Filo olmadığından o zaman ucuz; delegasyon milestone'unda ve D0 sahada yetersiz
  kalırsa yapılır.
- **Öneri:** M7P6F, bekleyen RAK4630 rebuild + audit uzlaşmasından sonra merge edilmeli; bu
  analiz ona engel değil.

## 24. Şimdi dondurulması önerilen değişmezler

1. Gateway yetkisi yalnız backend'den türetilir ve capability ile kapsamlıdır; PIN, BLE bond,
   konum veya donanım kimliği asla yetki değildir.
2. Her kriptografik gönderenin kendi anahtar bağlamı vardır (M7P6D); site/filo anahtarı yok.
3. Tracker kalıcı güvenlik durumu gateway sayısıyla ölçeklenmez; gateway başına durum varsa önbellektir.
4. Capability'ler kriptografik olarak bağlıdır (KDF/AAD), yalnız UI'da değil.
5. Güvenlik duvar saatine dayanmaz; expiry tracker monoton sayacına veya olaylara bağlıdır.
6. Gateway factory reset tüm grant malzemesini yok eder; backend bir nesli yeniden vermez;
   gateway güvenlik durumu yedeklenmez.
7. Çevrimdışı delege komut tracker'ın taze challenge'ına bağlıdır (ilk sürümde store-forward yok).
8. Replay ≠ idempotency; değişiklik yapan komutlar command_id + versiyon/CAS kullanır.
9. "Başarılı" yalnız kimliği doğrulanmış RESULT ile; TX_DONE ≠ teslim.
10. Kimliği doğrulanmamış girdi flash'a yazamaz (mevcut kural).

## 25. Dondurulmaması gerekenler

Wire baytları; KDF etiket dizeleri; grant paketi formatı; capability bitmap'i; expiry birimi
ve marjları; D0/D1 seçimi; önbellek slotları; oturum sayacı genişliği; rendezvous hash
girdileri; RX penceresi; PIN uzunluğu/backoff sabitleri; okuma delegasyonu; C/EDHOC seçimi;
`K_root` mu türetilmiş `K_deleg` mi saklanacağı (custody).

## 26. Son tura kalan iki mimari

1. **H = A + D** (türetilmiş delegasyon).
2. **H′ = A + C2** (backend sertifikalı gateway + EDHOC/CASE benzeri oturum).

Kararı değiştirecek saha varsayımları:

- Backend tracker sırrından türetim yapabilir mi? (Simetrik D2A için zaten gerekli.) Evet → H.
- Çevrimdışı gateway, son senkronizasyonundan sonra kaydedilen tracker'lara komut vermek
  zorunda mı? Evet → H′.
- Gateway'ler kiracılar arasında paylaşılacak / üçüncü taraf gateway olacak mı? Evet → H′.
- PHY daha hızlı SF'ye (SF7–9) geçecek mi? Geçerse C'nin RF maliyeti kabul edilebilir.
- RAK gateway'de ≈ 32 KB delegasyon depolaması ayrılabilir mi? Hayırsa H′ veya telefonda
  gateway'e sarılı bölünmüş saklama.

## 27. Önerilen prototip yönü

**H, D0 varyantıyla** (gateway başına tracker durumu yok; iptal = sayaç expiry + backend
yenilemeyi keser) prototip/audit aşamasına geçirilmeli:

- Mevcut simetrik kök, M7P6D KDF/CCM adayı, CC310'da kanıtlanmış HKDF/CCM ve M7P6F olduğu
  gibi kullanılır; yeni primitif, ECC koeksistans riski veya sertifika parser'ı yok.
- Yeni gateway için sıfır RF, tracker'da sıfır yazım.
- Nonce güvenliği reset/restore/klona karşı yapısal.
- Canlı relay penceresine sığar.
- En yakın standart karşılıklar: ACE-OSCORE (RFC 9203) + Echo (RFC 9175) + OSCORE Ek B.2,
  LoRaWAN 1.1 Join Server türetilmiş anahtar dağıtımı, Z-Wave wake-up kuyruğu ve SPAN alıcı entropisi.

Sıra (her biri ayrı, sahip onaylı slice):

1. ADR değişikliği: "delege gateway" sınıfı.
2. Host üzerinde KDF/oturum vektörleri.
3. RAK KAT.
4. Pencere zamanlama simülasyonu.
5. Gerçek donanımda doğrudan ve canlı relay ölçümü.

D1, çalıntı gateway senaryosu sahada ölçülüp gerekli görülürse eklenir.
