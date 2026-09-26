# Bağımsız Re-audit — ORUN TLP v2 Delegated Command Security Contract V2

Status: **INDEPENDENT RE-AUDIT RESULT — PASS WITH FIXES (V2 may proceed to owner approval after listed document fixes; not yet wire-freeze-ready).**
Documentation-only; no firmware, test, fixture or protocol byte was changed.

- Audit branch: `design/tlp-v2-command-security-contract`
- Prompt: `docs/audits/TLP_V2_COMMAND_SECURITY_CONTRACT_REAUDIT_PROMPT.md`
- Expected HEAD in prompt: `2b6bcf876dcfee2d4e24fb07af6216a0d18e6b0f`
- Actual HEAD audited: `44ead40468e5f9a3374b9656fe48d77b06339212` (adds only the re-audit prompt file on top of `2b6bcf8`; reviewed content is identical to `2b6bcf8`)
- Baseline main: `e2a370510c595c5f4b88e94a1212fb95d848a273`
- Primary target: `docs/architecture/ORUN_TLP_V2_COMMAND_SECURITY_CONTRACT_DRAFT_V2.md`
- Previous audit: `docs/audits/TLP_V2_COMMAND_SECURITY_CONTRACT_INDEPENDENT_AUDIT.md` (target `3af79af`)
- Re-audit date: 2026-09-26

Evidence boundary: this re-audit is based only on documentation and source
reading. No host test, build, RF or power measurement was run. Airtime values
are calculated, not measured. CSPRNG quality, APPROTECT, gateway platform and
backend key custody are **UNKNOWN**.

---

## A. Verdict

**PASS WITH FIXES.** Bu sürüm V2 mimarisini owner onayına sunulabilir kılıyor, ancak wire freeze için henüz hazır değil.

- **HEAD kontrolü:** Prompt `2b6bcf8` bekliyordu, gerçek HEAD `44ead40`. Aradaki tek commit yalnızca prompt dosyasını ekliyor (`git diff --stat 2b6bcf8 HEAD` = 1 dosya). Denetlenen içerik `2b6bcf8` ile birebir aynı. `3af79af..2b6bcf8` aralığında firmware, protokol kodu veya fixture değişikliği yok.
- **Eski bulgular:** Önceki denetimdeki 9 HIGH bulgunun hepsi tasarım düzeyinde kapatılmış ya da doğru biçimde ayrı bir dilime ertelenmiş.
- **frame_key_salt:** Kriptografik olarak **güvenli**. Nonce reuse sınıfını, iki bağımsız arızanın (counter rollback + salt tekrarı) aynı anda gerçekleşmesini gerektiren bir düzeye indiriyor. Bunun için CSPRNG kaynağına dair ek kurallar şart (N5).
- **Yeni bulgular:** BLOCKER veya HIGH yok. Beş MEDIUM bulgu var ve bunlar wire freeze'i ya da ilgili implementasyon dilimini bloke ediyor:
  - N2: 56 baytlık header her context'e uygulanırsa tracking ve telemetri airtime'ı şişer.
  - N3: Gateway timeout'u ile relay retention arasındaki ilişkiden sahte "başarısız" durumu doğabilir.
  - N4: Atomik slot kaydı invariant'ı yazılmamış.
  - N5: CSPRNG kaynak kuralları eksik.
  - N6: Custodian seçim mekanizması tanımsız.

## B. Eski bulguların durumu

| ID | Durum | Kısa gerekçe |
|---|---|---|
| F1 | CLOSED | Reserve block 1; retry = yeni counter ve 1 quota birimi; byte-identical yeniden gönderim counter tüketmiyor (§5.1). |
| F2 | CLOSED | Quota grant başına tek değer; aynı grant'teki tüm scope'lar tek counter ve tek HWM paylaşıyor (§5). |
| F3 | CLOSED | Floor artık yalnızca kaldırma veya compromise durumunda ilerliyor. Boş slot varken yeni gateway eklemek ve re-enroll floor'u değiştirmiyor; bunlar per-gateway `grant_generation` ile yönetiliyor (§4.1). |
| F4 | PARTIALLY CLOSED | `GATEWAY_POLICY_FLOOR_ADVANCE` A2D işlemi adlandırılmış, ama "future" olarak bırakılmış. Monoton `max()` semantiği ve etki alanı beyanı (quota × tracker sayısı × grant scope'ları) eksik. |
| F5 | CLOSED | `opcode → scope` registry'si tanımlı ve zorunlu kontrol sırası verilmiş (§8, §11 adım 15). |
| F6 | CLOSED (model düzeyinde) | Grant'e PoP, hedef/tenant bağı, monoton generation ve online iptal kontrolü eklenmiş (§10). Kalan ayrıntılar F bölümünde. |
| F7 | CLOSED | Nonce reuse per-frame salt ile çözülmüş (D bölümü); N5 koşuluyla. |
| F8 | CLOSED | §9.1: floor/slot durumu yalnızca yeni credential lifetime ile sıfırlanabilir; yer SecurityStore v3. |
| F9 | CLOSED (ilk dilim) | Relay custody RAM-only; doğrulanmamış girdi flash'a hiç yazılmıyor. |
| F10 | CLOSED | Dedupe tag'i de kapsayan tam frame kimliği üzerinden yapılıyor (§18). |
| F11 | CLOSED | Gateway başına, tracker başına tek outstanding frame (§7.3). Terminal durumun anlamı N3'te ele alınıyor. |
| F12 | PARTIALLY CLOSED | "Tek custodian" politikası konmuş ama seçim mekanizması yok (N6). |
| F13 | CLOSED | §11'deki 18 adımlık sıra doğru. HWM sıfırlamasının atomikliği N4'te. |
| F14 | PARTIALLY CLOSED (doğru erteleme) | `generation_` CAS token'ı olarak reddedilmiş, token ayrı bir dilime bırakılmış. COMMAND layout'u provisional. |
| F15 | PARTIALLY CLOSED | `request_counter` ve backend'e opaque RESULT iletimi eklenmiş. Byte layout'u açık. |
| F16 | PARTIALLY CLOSED | Tam zincirin bütçelenmesi gerektiği söylenmiş ama hesap yapılmamış. Hesap J bölümünde. |
| F17 | CLOSED | Yasaklı aileler listelenmiş (§12.2). |
| F18 | PARTIALLY CLOSED | `header_len` kaldırılmış; `path_flags` ve context/family matrisi eklenmiş. A2D/D2A layout'ları ve ayrılmış değerler (`0xFFFFFFFF`) açık (N2 ile ilişkili). |
| F19 | CLOSED | §13 `command_id` semantiğini açıkça tanımlıyor. |
| F20 | CLOSED | Backend en fazla 4 grant veriyor; duplicate slot ve azalan HWM kurtarmada FAULT. |
| F21 | OPEN (LOW, bloklayıcı değil) | ConfigStore aşınma/hız limiti ve `requestSave()` no-op/async belirsizliği; config dilimine ait. |
| F22 | OPEN (LOW) | Görünür metadata ile trafik analizi hâlâ yazılı değil. Salt yeni bir sızıntı eklemiyor. |
| F23 | PARTIALLY CLOSED | `ORUN_PROTOCOL_EVOLUTION_PLAN.md:233` "gateway … does not own keys, counters" diyor; delegated istisnası eklenmemiş. |
| F24 | OPEN (INFO) | Backend PRK/HSM custody bağımlılığı V2'de adlandırılmamış (UNKNOWN). |
| F25 | PARTIALLY CLOSED | §20 gateway platform incelemesini zorunlu kılıyor; partition hâlâ tanımsız. |

## C. Yeni bulgular

### N1 — LOW — HKDF-Extract gereksiz; daha basit standart kullanım mümkün

- **Kanıt:** §6.1'de `frame_prk = HKDF-Extract(salt = frame_key_salt, IKM = K_grant)` var. Görünür salt HMAC *key* konumunda, gizli `K_grant` ise message konumunda. Güvenlik HMAC'in dual-PRF varsayımına dayanıyor; bu, TLS 1.3'te de kullanılan yaygın bir kabul.
- **Arıza senaryosu:** Kırık değil. Yalnızca gereksiz bir varsayım ve fazladan bir HMAC hesabı getiriyor.
- **Düzeltme:** `K_frame = HKDF-Expand(K_grant, "ORUN-TLP-V2-GW-FRAME-v1" || direction || frame_key_salt, 16)`. `K_grant` 32 bayt ve düzgün dağılımlı olduğu için RFC 5869 anlamında geçerli bir PRK. Gizli değer HMAC key konumunda kalır ve yalnızca standart PRF varsayımı gerekir.
- Ayrıca `K_grant = HKDF(...)` ifadesindeki "..." yerine açıkça "M7P6D PRK (salt = `credential_id`, IKM = `K_root`) + Expand" yazılmalı.
- **Implementation-blocking:** HAYIR (KDF test vektörleri dondurulmadan önce yapılmalı).

### N2 — MEDIUM — 56 baytlık header'ın tüm context'lere uygulanıp uygulanmadığı belirsiz

- **Kanıt:**
  - §14.1'de SECURE_APP header'ı sabit 56 bayt.
  - §14.2 yalnızca "non-delegated contexts … according to that context's later frozen layout" diyor.
  - Floor (4), generation (4), salt (12) ve grant_flags (1) alanları yalnızca delegated context'lerde anlamlı.
- **Arıza senaryosu:**
  - Gelecekteki D2A position/telemetri trafiği type 0x01 ile bu 56 baytlık header'ı kullanırsa, yaklaşık 20 baytlık payload 84 B'lık bir frame'e (≈1,97 s) dönüşür. Bu, v1 POSITION'ın (0,99 s) yaklaşık 2 katı.
  - 100 tracker ve 15 dakikalık cadence'de kanal yükü %11'den yaklaşık %22'ye çıkar. Bu, relay forwarding ve retry eklenmeden önce pure ALOHA'nın ~%18'lik tavanını aşıyor.
  - Böylece komut çerçevesi, product architecture'ın yasakladığı "monolithic universal payload"a dönüşmüş olur.
- **Düzeltme:** Wire freeze'den önce bir karar yazılsın. İki seçenek:
  - (a) type 0x01'i delegated çerçeve olarak sabitle; A2D ve D2A kendi kompakt type'larını alsın.
  - (b) header uzunluğu, byte 2'deki `security_context` değerine göre deterministik olarak belirlensin.
- **Implementation-blocking:** EVET (wire freeze).

### N3 — MEDIUM — Gateway'in "terminal" durumu ile relay retention süresi uyumsuz olabilir

- **Kanıt:** §7.3'e göre bir sonraki counter "terminal retry/RESULT state"ten sonra üretiliyor. §17'ye göre relay kendi saklama süresini ve deneme sayısını bağımsız olarak uyguluyor.
- **Arıza senaryosu:**
  1. Gateway timeout nedeniyle N numaralı komutu "başarısız" ilan eder.
  2. Relay N'yi saklamaya devam eder ve tracker'a daha sonra teslim eder; tracker N'yi uygular.
  3. Kullanıcı, uygulanmış bir komutu başarısız görür (sahte başarı değil, sahte başarısızlık).
  4. Gateway'in N+1 komutu daha önce ulaşmışsa N reddedilir; bu durum güvenli.
- **Düzeltme:** Terminal durum "UNCONFIRMED" olarak adlandırılsın, "FAILED" olarak değil. Ya da gateway timeout'u, relay'in maksimum retention süresi ile teslim penceresinin toplamından büyük olsun. CAS korumalı retry'lar zaten güvenli.
- **Implementation-blocking:** EVET (gateway gönderici ve UI dilimi).

### N4 — MEDIUM — Slot kaydının atomikliği invariant olarak yazılmamış

- **Kanıt:** §4.1 "higher generation authenticated and durably committed" diyor; §9'a göre byte layout'u sonraki dilimde belirlenecek.
- **Arıza senaryosu:** Generation ve HWM iki ayrı kayıt olarak yazılırsa ve önce "HWM = 0" commit edilip ardından güç kesilirse, slot eski generation ile HWM 0 durumunda kalır. Bu durumda eski generation'ın 1..900 aralığındaki counter'ları yeniden geçerli olur. Bu bir **resurrection** (eski komutun yeniden geçerli hale gelmesi).
- **Düzeltme:** Şu invariant yazılsın: `(slot.gw_id, gen, replay_bound)` tek bir commit-last kayıttır. Floor artışı ile lower-floor slotların geri kazanımı da tek bir atomik geçiştir (ya tek kayıt ya da activation-last A/B). Recovery, bir slotun `gen/floor`'u ile HWM'i uyumsuzsa FAULT vermeli.
- **Implementation-blocking:** EVET (SecurityStore v3 dilimi).

### N5 — MEDIUM — Salt'ın güvenliği için CSPRNG kaynak kuralları eksik

- **Kanıt:** §6.1 yalnızca "physically validated CSPRNG path" diyor.
- **Arıza senaryosu:**
  - Gateway salt'ı, seed'i flash'ta saklanan bir yazılım DRBG'sinden üretirse, klon veya yedekten geri yükleme **seed'i de kopyalar**. İki cihaz aynı salt dizisini üretir ve counter'lar da örtüşür; nonce reuse geri döner.
  - RNG arızası (örn. sabit çıktı) sessiz kalırsa koruma yalnızca counter'a iner.
  - Tracker D2GW RESULT'ı için de salt üretmek zorunda. M7P6D, Adafruit RNG'nin Bluefruit ile paylaşılan global state'ini inceleme konusu olarak işaretlemişti.
- **Düzeltme:**
  - Salt yalnızca donanım TRNG'sinden alınsın ya da her boot'ta TRNG'den yeniden seed edilen bir DRBG'den üretilsin. Kalıcı seed tek başına kaynak olmasın.
  - RNG sağlık testi başarısız olursa delegated TX fail-closed olsun.
  - Tracker RNG'si Bluefruit/CC310 coexistence KAT'ına dahil edilsin.
- **Implementation-blocking:** EVET (§22 adım 4 ve 6).

### N6 — MEDIUM — Tek custodian kuralının mekanizması yok

- **Kanıt:** §18 "initial topology uses a single selected custodian" diyor, ama gateway→relay bacağında custodian'ı belirten bir alan veya yapılandırma kuralı yok.
- **Arıza senaryosu:** `relay_allowed = 1` işaretli bir frame'i duyan her relay custody alır. Gateway da doğrudan erişimdeyse tracker'ın aynı RX penceresinde birden fazla verici aynı anda yayın yapar ve teslim başarısız olur.
- **Düzeltme:** İlk dilim için yapılandırma tabanlı bir kural yeterli:
  - Site başına en fazla bir custody-enabled relay olsun.
  - Custodian tanımlıysa gateway doğrudan teslim yapmasın.
  - Wire değişikliği gerekmiyor.
- **Implementation-blocking:** EVET (relay dilimi); SECURE_APP freeze'ini bloke etmez.

### N7 — LOW — Floor advance işleminin semantiği tanımsız

- **Kanıt:** §4.2'de işlemin davranışı belirtilmemiş.
- **Arıza senaryosu:** Relay'de gecikmiş bir "floor = 12" frame'i, tracker floor 13'e geçtikten sonra teslim edilir. Bunun hata mı no-op mu olduğu belirsiz.
- **Düzeltme:** Semantik `floor = max(floor, X)` olsun: idempotent ve monoton. Bu sayede gecikmiş store-forward güvenlidir, ayrı bir tazelik kuralı gerekmez. Kalan gateway'lerin floor itebilmesi için registry'ye no-op/read-only bir SYNC opcode'u eklensin.
- **Implementation-blocking:** HAYIR.

### N8 — LOW — PoP imzası komutun içeriğini kapsamalı

- **Kanıt:** §10 PoP'u "fresh gateway challenge" olarak tanımlıyor.
- **Arıza senaryosu:** Aynı oturumda BLE linkinde MITM varsa, bir PoP başka bir komut için yeniden kullanılabilir.
- **Düzeltme:** İmza `challenge || command_digest(target, opcode, args, command_id)` üzerinden atılsın.
- **Implementation-blocking:** HAYIR (user-auth dilimine ait).

## D. frame_key_salt kriptografik analizi

1. **Stale gateway rollback'inde nonce reuse'u önlüyor mu?** **EVET**, sağlıklı ve TRNG kaynaklı bir CSPRNG varsayımıyla. Rollback aynı counter'ı tekrar üretse bile her yeni frame yeni bir salt ve dolayısıyla yeni bir `K_frame` kullanır; aynı `(key, nonce)` çifti oluşmaz. Her `K_frame` fiilen tek bir şifreleme için kullanılıyor.
2. **Domain separation doğru mu?** **EVET.**
   - `"ORUN-TLP-V2-GW-GRANT-v1"` ve `"…-GW-FRAME-v1"` etiketleri, M7P6D'nin `"ORUN-TLP-V2-AEAD"` etiketinden 12. baytta ayrılıyor ('G' ≠ 'A').
   - GRANT ve FRAME etiketleri birbirinden 16. baytta ayrılıyor.
   - Tüm alanlar sabit uzunlukta, bu yüzden kodlama injektif.
   - Yön bilgisi `frame_info` içinde; GW2D ve D2GW anahtarları ayrı.
   - Tek not N1: Extract yerine yalnızca Expand kullanmak daha sade.
3. **Saldırganın salt'ı kontrol etmesi zarar verir mi?** **HAYIR.**
   - Salt anahtar *seçimini* değil, anahtar *türetmeyi* etkiliyor. Grant seçimi gw, floor, gen, scope ve quota alanlarıyla yapılıyor.
   - HMAC tabanlı türetmede related-key açığı yok.
   - CPU maliyeti: `K_grant` her grant için önbelleğe alınabilir. Frame başına 2 HMAC ve 1 CCM gerekiyor; 1,5 saniyelik minimum RF süresiyle sınırlı.
   - Replay kabulü salt'tan bağımsız: counter ve HWM AEAD'den sonra kontrol ediliyor.
4. **Salt AAD'de olmalı mı?** Kesin gerekli değil; salt değişirse anahtar değişir ve tag zaten geçmez. Yine de AAD'de **kalmalı**: maliyeti yok, katmanlı savunma sağlıyor ve dedupe kimliğini bağlıyor. CCM key-committing değil, ama bunu ancak `K_grant`'e sahip bir gateway sömürebilir ve o zaten yetkili.
5. **CSPRNG salt'ı tekrarlarsa?** Counter farklı olduğu sürece nonce farklı kalır ve güvenlik sağlanır. Reuse için rollback ile salt tekrarının **birlikte** olması gerekir. 96 bit rastgele salt ve grant başına ≤1023 frame için çakışma olasılığı ≈ q²/2⁹⁷, pratikte ihmal edilebilir. RNG'nin sistematik arızası (sabit çıktı) N5 sağlık testiyle yakalanmalı.
6. **Byte-identical retransmission orijinal frame'i mi yeniden kullanıyor?** **EVET.** §5.1 ve §6.1 aynı salt, aynı anahtar ve aynı ciphertext'in gönderildiğini söylüyor. §20 bekleyen frame baytlarının kalıcı saklanmasını istiyor. Frame baytları kaybolursa yeni counter ve yeni salt ile yeniden oluşturuluyor; bu güvenli.
7. **Gateway klonu hâlâ nonce reuse yaratabilir mi?** Yalnızca N5 ihlal edilirse, yani kalıcı DRBG seed'i klonlanırsa. N5 uygulanırsa klon yalnızca **yetki kopyası ve availability** sorunu olur: counter'lar çakışır, tracker ilk geleni kabul eder, diğerlerini reddeder.
8. **Daha basit bir yapı mümkün mü?**
   - Evet, N1'deki Expand-only yapı. Nonce yapısı M7P6D ile aynı kalır.
   - Sabit anahtar ile 13 baytlık rastgele nonce da güvenli olurdu, ama M7P6D'nin counter tabanlı nonce yapısından sapar ve boyut kazancı sağlamaz.
   - AES-SIV gibi nonce-misuse dirençli bir AEAD için CC310'da kanıtlanmış bir yol yok.
   - **Sonuç: Nonce ve anahtar güvenliği EVET**, N1 ve N5 düzeltmeleriyle.
   - **Bedeli:** Salt ve genişletilmiş generation alanları frame başına +16 B, yani relay'li bir komut zincirinde yaklaşık +1,3 s airtime.

## E. Replay / iptal / crash-order analizi

| Durum | Sonuç |
|---|---|
| Eski gateway'ler offline iken yeni gateway ekleme | SAFE. Boş slot varsa floor değişmiyor; eski gateway'lerin yetkisi sürüyor. |
| Factory reset sonrası re-enroll | SAFE. Gen artıyor; relay'de kalmış eski gen frame'leri, yeni gen kabul edildikten sonra reddediliyor. |
| Gateway iptali / ele geçirilmesi | Sayı olarak sınırlı, zaman olarak sınırsız bir artık risk var: quota × gateway'in hizmet verdiği tracker'lar × grant scope'ları. Floor advance ile kapanıyor (N7). |
| Floor advance frame'inin relay'de gecikmesi | SAFE, `max()` semantiği ile. O arada iptal edilmiş gateway'in quota'sı kadar risk kalıyor. |
| Floor commit sırasında, aktivasyondan önce güç kaybı | Eski floor ve slotlar bozulmadan kalıyor, dispatch yapılmamış. SAFE. |
| Floor commit sırasında, aktivasyondan sonra güç kaybı | Daha düşük floor'lar reddediliyor. SAFE. |
| A2D HWM ve floor ayrı kayıtlarda | Floor artışı idempotent olduğu için her iki commit sırası da güvenli. |
| Yüksek gen kabul edildikten sonra reboot | Kalıcı `(gen, bound)` korunuyor; eski gen reddediliyor. SAFE, **ancak N4 atomikliği sağlanırsa**. |
| Reboot sonrası eski gen replay'i | Reddediliyor. |
| Slot geri kazanımı | Yalnızca floor ilerlediğinde yapılıyor ve düşük floor kalıcı olarak reddediliyor. Resurrection yok. |
| Dört slot doluyken 5. gateway | Kripto öncesi (adım 6) reddediliyor, mutasyon yok. |
| Tracker security store bozulması | FAULT, delegated alım fail-closed. Çıkış yolu yalnızca yeni credential. SAFE. |
| Yalnızca delegated durumu silen reset | §9.1 yasaklıyor. v3'te bu işlem için bir API bulunmamalı. Partition'ın fiziksel silinmesi credential'ı da siler. SAFE. |

**Sonuç:** Kabul edilmiş eski bir komut yeniden geçerli **olamaz**. Bunun iki koşulu var: N4'teki atomiklik invariant'ı ve §9.1. İkisi de v3 şemasında zorunlu olmalı.

**Quota ve retry:**

- Reserve block 1 tutarlı.
- Her yeni deneme 1 counter ve 1 quota birimi tüketiyor.
- Byte-identical yeniden gönderim hiçbir şey tüketmiyor.
- Aynı `command_id` yeni counter ile retry edilebiliyor.
- Quota yenilemesi yalnızca o gateway'in gen'ini değiştiriyor.
- Kötü niyetli counter atlamaları yalnızca o gateway'in kendi slotunu tüketiyor; başka gateway'lerin yetkisini etkilemiyor.
- Default quota değeri politika olarak seçilmeli.

## F. Offline kullanıcı yetkisi analizi

Model **ayrı bir implementasyon dilimine geçmek için yeterli**:

- Grant backend tarafından imzalı.
- App'in public key'i grant'e bağlı.
- Gateway taze bir challenge ile PoP istiyor.
- Grant tenant/site/hedef kümesine bağlı.
- Gateway en yüksek generation'ı saklıyor ve düşükleri reddediyor.
- Internet varken online iptal kontrolü yapılıyor.
- Offline durumda bütçe sınırlı ve retry'lar bütçe tüketmiyor.
- Tracker kullanıcı bilgisi görmüyor.

Kalan işler (hiçbiri bloklayıcı değil):

- N8: PoP imzası komut özetini kapsamalı.
- Bütçenin birimi (mantıksal komut) ve kapsamı (grant × gateway) netleştirilmeli.
- Gateway'in offline durumda "hedef kümesi" sorusunu yanıtlayabilmesi için tracker üyelik bilgisi enrollment sırasında gelmeli.
- İmza şeması ve verify yolu henüz UNKNOWN.

Artık risk Σ(gateway bütçeleri) kadar ve açıkça dokümante edilmiş.

## G. Komut tazeliği / CAS analizi

**Sınır doğru çizilmiş.** Mevcut `ConfigStore::recover()` bozulma durumunda N−1'e veya 0'a düşebiliyor, dolayısıyla `generation_` ABA'ya açık. Bunun CAS token'ı olarak reddedilmesi doğru. Sıralama (önce `ALREADY_SATISFIED`, sonra `STALE`, sonra uygula) RESULT kaybı sonrası retry'ı flash yazmadan çözüyor.

Config state token'ının minimum özellikleri:

1. Aynı credential lifetime içinde, bozulma, fallback, reset, migration ve firmware downgrade sonrasında **asla tekrarlanmamalı**.
2. Her kalıcı içerik değişikliğinde ve varsayılana düşüldüğünde değişmeli.
3. Değişmeyen veya başarısız kayıtta değişmemeli.
4. Minimal yapı: `lineage32 || gen32` (64 bit). `lineage`, recovery sürekliliği kanıtlayamadığında TRNG'den yeniden üretilir; bu durumlar: kayıt yok, bozuk sayfa görüldü, schema migration. ABA olasılığı ≤2⁻³².
5. Bu nedenle COMMAND'daki precondition alanı muhtemelen 8 bayt olmalı; args üst sınırı ≤12 bayt olur. COMMAND ve RESULT layout'larının provisional kalması doğru.

Desired-state config için genel bir kalıcı command journal **gerekmiyor**; CAS ile kalıcı sonuç durumu yeterli.

## H. Wire / AAD incelemesi

**56 baytlık header:**

- Alan sınırları ve uzunluk tutarlı: 64 + N ≤ 96, N ≤ 32.
- Tüm alanlar big-endian; integer promotion sorunu yok.
- AAD bayt 0..55'i kapsıyor, salt dahil.
- CCM: 13 baytlık nonce, L = 2, 56 B AAD ve 32 B payload limitlerin çok altında. 8 baytlık tag ile forgery olasılığı 2⁻⁶⁴.
- v1/v2 ayrımı bayt 0 üzerinden yapılıyor, belirsizlik yok.

**Eksikler:**

- N2: Header'ın context'e göre belirlenmesi kararı.
- `0xFFFFFFFF` değerlerinin floor, gen ve `key_epoch` için geçersiz olarak ayrılması.
- `counter ≥ 1` kuralı.
- `ciphertext_len` için aile minimumu (COMMAND ≥16).

**96 baytlık maksimum iç frame** kabul edilebilir.

**112 baytlık relay wrapper:**

- Kurallar doğru: iç içe wrapper reddi, `relay_allowed` kontrolü, değişmemiş iç frame, tag'i de kapsayan dedupe.
- Wrapper alanları güvenilmeyen gözlem verisi.
- Relay'den tracker'a teslim wrapper ile yapılıyor (+16 B, ≈0,33 s). One-hop kuralının korunması için bu kabul edilebilir.

**Kalıcı custody'nin ertelenmesi SECURE_APP'i değiştirir mi?** **HAYIR.** İleride eklenecek bir admission MAC, değişmemiş SECURE_APP'i saran ayrı bir outer type olarak tanımlanabilir.

**Hardware-neutral mi?** Evet. Alanlar kimlik, sayaç, dBm ve dB; SX126x'e özgü kodlama yok. 64 bitlik legacy device ID namespace'i artık KDF'de de kullanılıyor; bu bilinçli bir namespace freeze olarak kaydedilmeli.

## I. Relay custody / DoS analizi

- RAM custody limitleri (hedef başına 1–2, relay başına toplam 8, deneme ve retention üst sınırları) DoS'u sınırlıyor.
- Sahte RF trafiği flash'a **hiç** yazılamıyor; flash aşınması sıfır.
- Saldırgan kuyruğu doldurup meşru komutu geciktirebilir. Bu, jamming ile eşdeğer bir RF DoS; kabul edilen bir sınır.
- Dedupe zehirlenmesi kapandı: saldırgan, önceden bilinmeyen tag'i taklit edemez.
- Relay reboot'unda RAM custody'nin kaybı dürüstçe bir availability kaybı olarak sınıflandırılmış.
- Açık kalanlar: N6 (custodian seçimi) ve N3 (gateway timeout'u ile retention ilişkisi).
- Kalıcı custody, SECURE_APP'e dokunmadan ertelenebilir.

## J. Airtime ve kaynak analizi

**Varsayımlar:** SF11, BW125, CR 4/5, LDRO = 1, explicit header, CRC açık, preamble 8. Değerler hesaplanmıştır, RF ölçümü değildir.

`Tsym = 2¹¹/125000 = 16,384 ms`

`T = (8 + 4,25)·Tsym + [8 + ceil((8PL − 4SF + 28 + 16) / (4(SF − 2)))·5]·Tsym`

| Frame | Boyut | Airtime |
|---|---:|---:|
| V2 COMMAND/RESULT (N = 24) | 88 B | 1970,176 ms |
| Maksimum iç frame | 96 B | **2134,016 ms ✓** |
| Wrapper (N = 24) | 104 B | 2297,856 ms |
| Maksimum wrapper | 112 B | **2379,776 ms ✓** |
| Karşılaştırma: aynı komut 40 B header ile | 72 B | 1642,5 ms |

Taslaktaki değerler doğru.

**Komut zincirleri:**

- Doğrudan (komut + RESULT): **≈3,94 s**
- Relay üzerinden (GW→R, R→T, T→R, R→GW): **≈8,54 s**
- Relay üzerinden, iki ek teslim denemesiyle: ≈13,1 s

| Cihaz | Relay'li komut, 1/gün | 10/gün | Tüm siteye tek saatte komut | Arka plan v1 POSITION, 15 dk / 5 dk |
|---:|---:|---:|---:|---:|
| 10 | %0,10 | %0,99 | %2,4 | %1,1 / %3,3 |
| 30 | %0,30 | %3,0 | %7,1 | %3,3 / %9,9 |
| 50 | %0,49 | %4,9 | %11,9 | %5,5 / %16,5 |
| 100 | %0,99 | %9,9 | **%23,7** | %11,0 / **%32,9** |

**Değerlendirme:**

- 10 ve 30–50 cihazda seyrek komut trafiği sorunsuz.
- 100 cihazda "tüm siteye tek saatte komut" senaryosu, 15 dakikalık tracking trafiğiyle birlikte ALOHA sınırını aşıyor. Toplu komutlar birkaç saate yayılmalı.
- Duty cycle varsayımı: 869,4–869,65 MHz bandında %10. Bu durumda bir gateway saatte ≤182 komut, bir relay saatte ≤156 teslim yapabilir.
- Tek SF11 kanalını asıl dolduran komutlar değil, tracking cadence'i. N2 bu yüzden önemli.

**Kaynaklar (tahmini):**

- **Tracker:** v3'te floor + 4 slot, slot başına yaklaşık 50–60 B'lık kayıt; RAM < 1 KB. Tracker D2GW RESULT'ları için RNG kullanıyor (N5).
- **Gateway (100 tracker):** Tracker başına `K_grant` (scope başına 32 B), bekleyen frame (≤96 B) ve sayaçlar, toplam ≈20–25 KB. Partition tanımsız (F25).
- **Relay:** RAM'de yaklaşık 8 × 112 B + dedupe; flash kullanımı yok.

## K. Owner onayı öncesi minimum düzeltmeler

1. **N2:** Header'ın context'e göre belirlenmesini veya delegated-only type kararını yaz.
2. **N4:** `(gw_id, gen, bound)` tek kayıt olsun; floor artışı ile slot geri kazanımı atomik bir geçiş olsun; uyumsuzlukta FAULT.
3. **N5:** Salt TRNG kaynaklı olsun (kalıcı seed tek başına kaynak olmasın), RNG sağlık testi başarısızsa fail-closed, tracker RNG'si coexistence KAT'ına dahil edilsin.
4. **N1:** Expand-only frame key; `K_grant` için Extract ve Expand baytları açıkça yazılsın.
5. **N7 ve F4:** `FLOOR_ADVANCE` için `max()` semantiği, SYNC opcode ve etki alanı beyanı.
6. **N3:** Terminal durum UNCONFIRMED olsun ya da gateway timeout'u relay retention'dan büyük olsun.
7. **N6:** Site başına tek custody-enabled relay; custodian varsa gateway doğrudan teslim yapmasın.
8. **F18:** Ayrılmış `0xFFFFFFFF` değerleri, `counter ≥ 1` kuralı, aile minimumları.
9. **F23 ve F24:** Protocol plan satır 233'e delegated istisnasını ekle; backend PRK custody'sini UNKNOWN bağımlılık olarak adlandır.

## L. Bu tasarımı bloke etmemesi gereken ertelenmiş işler

- Config state token dilimi ve COMMAND/RESULT plaintext'inin freeze'i.
- İmza şeması ve app/backend implementasyonu.
- N8 (PoP'un komut özetine bağlanması).
- Kalıcı relay custody ve admission MAC.
- Çok relay'li slotlama.
- Gateway platformu ve partition seçimi.
- ConfigStore aşınması ve API (F21).
- Metadata gizliliği (F22).
- Yasaklı ailelerin tazelik tasarımları.
- Fiziksel sleepy-tracker testi.
- 100 cihazdan büyük ölçek için domain partitioning.

## M. Nihai öneri

V2 henüz wire freeze'e geçemez. Doküman **DRAFT olarak kalmalı**. K1–K9 düzeltmeleri doküman düzeyinde ve küçük; tam bir yeniden denetim gerekmiyor. Bu düzeltmeler işlendikten sonra, dar kapsamlı bir doğrulama incelemesiyle şunlar **owner onayına uygun** hale gelir:

- delegated yetki mimarisi;
- iki seviyeli generation modeli;
- frame_key_salt yaklaşımı;
- 56 baytlık delegated SECURE_APP header'ı.

COMMAND ve RESULT plaintext'i config state token dilimine kadar provisional kalmalı.

**Kanıt sınırı:** Bu değerlendirme yalnızca dokümana ve koda dayanıyor. Host test, build, RF veya power ölçümü yapılmadı. Airtime değerleri hesaplanmıştır. CSPRNG, APPROTECT, gateway platformu ve backend custody UNKNOWN.
