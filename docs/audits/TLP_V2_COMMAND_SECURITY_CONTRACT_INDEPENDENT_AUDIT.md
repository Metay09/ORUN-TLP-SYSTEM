# Bağımsız Güvenlik / Protokol Denetimi — ORUN TLP v2 Delegated Gateway Command Security Contract

Status: **INDEPENDENT AUDIT RESULT — FAIL / REDESIGN REQUIRED (bounded, targeted).**
Documentation-only; no firmware, test, fixture or protocol byte was changed.

- Audit target branch: `design/tlp-v2-command-security-contract`
- Audit target HEAD: `3af79af8a52de779cedcfc05fc76c41e91fea5b5` (verified)
- Baseline main: `e2a370510c595c5f4b88e94a1212fb95d848a273`
- Document under audit: `docs/architecture/ORUN_TLP_V2_COMMAND_SECURITY_CONTRACT_DRAFT.md`
- Audit date: 2026-09-26

Evidence boundary: this audit is based only on documentation and source reading.
No host test, build or physical measurement was run. Airtime values are
calculated, not measured on RF. Duty-cycle values are an assumption. APPROTECT,
gateway platform and backend key custody are **UNKNOWN**.

---

## A. Yönetici kararı

**FAIL / REDESIGN REQUIRED** (kapsamı sınırlı, hedefli bir yeniden tasarım).

- Denetlenen commit: `origin/design/tlp-v2-command-security-contract` = `3af79af8a52de779cedcfc05fc76c41e91fea5b5` (doğrulandı). Tek üst commit `e2a3705`. Diff'te yalnızca taslak doküman var; firmware, test ve fixture dosyalarına dokunulmamış.
- Çekirdek model sağlam. Tracker başına HKDF delegasyonu, gateway başına kalıcı (durable) HWM, epoch tabanı (floor), AEAD sonrası replay commit ve CAS ile idempotent config yazımı birlikte çalışabiliyor. İstenen offline store-forward ürün davranışı **kurtarılabilir**; güvenliği sağlamak için bu davranıştan vazgeçmek gerekmiyor.
- Buna karşın şu parçalar kendi içinde çelişkili ya da eksik: quota, gateway_policy_epoch, reserve-ahead, scope↔opcode bağlama, OfflineUserGrant, relay custody ve key-reissue/rollback kuralları. Bu parçalarda **9 HIGH** bulgu var. Bunlardan biri (F7, doğru key context'inde nonce reuse) düzeltilmezse BLOCKER'a dönüşür.
- **Owner onayına veya wire freeze'e hazır değil.** Doküman DRAFT olarak kalmalı, düzeltilmeli ve yeniden denetlenmeli.

## B. Bulgular

### F1 — HIGH — Quota / reserve-ahead / yenileme

- **Kanıt:** §5'te `quota = maksimum sender counter` (15/63/255/1023) tanımlı. §7.1 "64'lük reserve-ahead, quota tavanında kırpılır; reboot kullanılmayanları yakar" diyor. §5'e göre quota yalnızca pe başına tanımlı.
- **Arıza senaryosu:**
  - quota=0 (15) veya 1 (63) iken ilk rezervasyon tüm quota'yı kapsar. Tek bir gateway reboot'u (veya brownout) o tracker için kalan offline yetkinin tamamını yakar. Rezervasyon tracker bağlamı başına tutulduğundan tek bir gateway güç kesintisi 1000 tracker'ın hepsinde aynı kaybı yaratır.
  - quota=2 dört reboot'ta, quota=3 on altı reboot'ta tükenir.
  - Offline yenileme yolu yok. Yenileme için yeni quota gerekir, yeni quota yeni KDF girdisi demek, o da §5'e göre yeni pe demek. Yeni pe ise aynı tracker'daki **diğer tüm gateway'leri** geçersiz kılar (bkz. F3).
- **Neden önemli:** Owner'ın açıkça istediği "Internet yokken enrolled gateway komut üretebilir" davranışı sıradan bir reboot ile çöküyor. Güvenlik doğruluğu bozulmuyor (burn güvenli), ama tasarım kendi bağlamında tutarsız.
- **Gerekli düzeltme:**
  - Quota "tracker'ın stateless olarak denetlediği maksimum counter" olarak kalsın (doğru seçim).
  - Gateway reserve bloğu 1 olsun (komut başına kalıcı yazma; komut düzlemi seyrek olduğu için aşınma önemsiz) ya da `min(4, kalan)` olsun.
  - Retry politikası ve "bir mantıksal komut kaç counter tüketir" tanımlansın.
  - Quota yenilemesi global floor'dan ayrılsın (F3).
- **Implementation-blocking:** EVET

### F2 — HIGH — quota/scope/counter semantiği kendi içinde çelişkili

- **Kanıt:**
  - §5: "backend quota'yı gateway **ve command scope**'a göre seçer; aktüasyon daha küçük quota alabilir."
  - §6: counter `(tracker, gw, pe, quota_code)` başına paylaşılıyor.
  - §7.1: counter `(tracker, gw, pe)` başına ve tüm scope'larda paylaşılıyor.
  - §9: slot `quota_code` saklıyor.
- **Arıza senaryosu:** Aynı gateway, scope A için quota=3 ve scope B için quota=0 ile ele alınsın.
  - Counter ortaksa, B'nin "15" tavanı paylaşılan counter 15'i geçtiği an B'yi kalıcı olarak kilitler.
  - Counter ayrıysa, iki bağımsız counter tek bir sıkı HWM'e akar ve karşılıklı reddedilir.
  - Implementer slot'u `(gw, quota)` ile anahtarlarsa, aynı gateway iki slot tüketir ("same gateway ID in two slots").
- **Neden önemli:** Stale-revocation sınırının tek dayanağı quota, ve bu mekanizmanın semantiği belirsiz.
- **Gerekli düzeltme:** Tek kural seçilsin. Öneri: quota `(tracker, gw, pe, gw_gen)` grant'i başına tek değer olsun ve tüm scope'lar aynı quota'yı paylaşsın. Scope başına farklı risk limiti ilk sürümde desteklenmesin. Slot quota saklamasın; quota kimliği doğrulanmış frame'den türetilsin.
- **Implementation-blocking:** EVET

### F3 — HIGH — Tracker başına tek global gateway_policy_epoch gereksiz geçersizleştirme yaratıyor

- **Kanıt:** §4 add/remove/reset/material değişikliğinin hepsinde pe ilerletiyor. §3.3 gateway factory reset'i de pe ilerletiyor.
- **Arıza senaryosu:** A pe 10'da offline. B'nin eklenmesi pe'yi 11'e taşır. B'nin ilk komutu tracker floor'unu 11'e çeker.
  - A'nın relay'lerde bekleyen tüm pe-10 komutları ve gelecekteki tüm komutları reddedilir. A ancak online olunca geri döner.
  - Bir site gateway'inin factory reset'i, o gateway'in hizmet verdiği tüm tracker'larda diğer tüm gateway'leri bozar. 1000 tracker × kalan gateway sayısı kadar anahtar yeniden dağıtılmalı, bu O(fleet) bir yük.
  - Senaryo 4 (sırasız teslimat): pe-12'deki C komutu relay'de beklerken B'nin pe-13 komutu önce ulaşırsa C kalıcı olarak ölür.
- **Neden önemli:** Owner yalnızca **yeni** gateway'in Internet gerektirmesini kabul etti. **Mevcut** offline gateway'lerin yetkisini kaybetmesini kabul etmedi.
- **Gerekli düzeltme (sınırlı, PKI değil):** İki seviyeli nesil.
  - `floor` (pe) yalnızca **gateway kaldırma** durumunda ilerlesin.
  - Slot'a monoton `gw_gen` eklensin. Re-enroll, factory reset sonrası yeniden kayıt ve quota yenilemesi yalnızca o gateway'in `gw_gen` değerini artırsın. Tracker slot içinde `gw_gen` < `slot.gw_gen` olanı reddetsin; daha yüksek `gw_gen` doğrulanıp kalıcılaştırıldıktan sonra HWM'i sıfırlasın.
  - Boş slot varsa gateway eklemek floor'u ilerletmesin.
  - Wire'da `authority_generation` 32 bit olarak kalabilir: `pe_be16 || gw_gen_be16`. KDF her ikisini de içersin.
- **Implementation-blocking:** EVET (wire alanı ve KDF girdisi etkileniyor)

### F4 — HIGH — İptalin aktif bir yayılma mekanizması yok; tek gateway'in ele geçirilmesinin etki alanı tanımsız

- **Kanıt:** §4'e göre tracker yeni epoch'u yalnızca "yeni epoch'ta başarıyla doğrulanmış delegated frame"den öğreniyor. §8 1000 tracker'lık materyalden söz ediyor.
- **Arıza senaryosu:** Çalınan gateway A iptal edilir ve backend pe'yi 12'ye çıkarır. Başka bir gateway o tracker'a komut göndermedikçe tracker 11'de kalır. A her tracker için, sahip olduğu **her scope**'ta, quota'ya kadar komut üretebilir; üst sınır ≈ 1000 × 1023. Seyrek komut alan tracker'larda pencere **zaman olarak sınırsız**. Referans gateway (RAK4631/nRF52840) güvenli anahtar deposu sunmuyor; APPROTECT durumu **UNKNOWN**.
- **Neden önemli:** Bu durum "fleet-wide secret yok" ilkesini anahtar düzeyinde ihlal etmiyor. Ancak **etki alanı fleet-wide**.
- **Gerekli düzeltme:**
  - (a) Backend A2D üzerinden kimliği doğrulanmış minimal bir `GATEWAY_FLOOR_ADVANCE` işlemi tanımlansın ya da kalan gateway'lerin gönderdiği no-op `POLICY_SYNC` komutu kullanılsın. M7P6F A2D replay yolu yeniden kullanılabilir.
  - (b) Gateway'lere yalnızca enrolled oldukları site/hesap kapsamındaki tracker'lar için materyal verilsin.
  - (c) Etki alanı ve 1000 tracker'a floor itmenin airtime maliyeti (≈1–2 saat, bkz. I) dokümana yazılsın.
- **Implementation-blocking:** EVET

### F5 — HIGH — scope↔opcode bağlama kuralı yok

- **Kanıt:** §6 "config anahtarı aktüasyon scope'u olarak yeniden etiketlenemez" diyor. §14'te `UNAUTHORIZED_SCOPE` var ama opcode'un scope'a ait olması gerektiğine dair bir kural yok.
- **Arıza senaryosu:** Config scope'unun anahtarıyla şifrelenmiş bir frame içine OPEN_BLE veya aktüasyon opcode'u konur. Tag geçerlidir ve tracker opcode'u çalıştırır.
- **Neden önemli:** Scope ayrımı anahtar düzeyinde var ama uygulama düzeyinde uygulanmıyor. Bu gerçekçi bir yetki atlatmasıdır.
- **Gerekli düzeltme:** Firmware'e sabit bir `opcode → required scope_id` tablosu eklensin. AEAD'den sonra ve dispatch'ten önce zorunlu olarak kontrol edilsin. Uyumsuzlukta `UNAUTHORIZED_SCOPE` dönülsün ve yan etki olmasın. Backend A2D (tam yetki) ayrıca tanımlansın.
- **Implementation-blocking:** EVET

### F6 — HIGH — OfflineUserGrant bir bearer token; hedef kümesine ve iptale bağlı değil

- **Kanıt:** §10'da bağlanan alanlar yalnızca `subject/generation/scope/budget/signature`.
- **Arıza senaryosu:**
  - (1) BLE/PIN kanalından, telefon yedeğinden veya kopyalanmış bir app'ten sızan grant başka bir telefondan tam yetkiyle kullanılır.
  - (2) Grant hedef tracker kümesini veya tenant'ı bağlamıyor. Paylaşılan altyapıda, 1000 tracker'ın anahtarını tutan bir gateway X kullanıcısının grant'iyle Y müşterisinin tracker'ına komut üretebilir.
  - (3) Online gateway da yalnızca imzayı doğruluyor. İptal edilmiş kullanıcı online bir gateway'de bile bütçe bitene kadar çalışır.
  - (4) Gateway kullanıcı başına görülen en yüksek generation'ı tutmuyor. Eski ama daha büyük bütçeli bir grant yeniden sunulabilir.
- **Neden önemli:** Gerçekçi yetki atlatması ve tenant karışması.
- **Gerekli düzeltme:**
  - Grant içinde app'e ait public key olsun ve gateway'in verdiği challenge'a standart bir imzayla proof-of-possession istensin. Android Keystore'da export edilemeyen anahtar kullanılsın. Özel imza şeması icat edilmemeli.
  - Hedef kümesi veya ownership group referansı bağlansın.
  - Gateway online iken backend'e danışsın ve iptal / `min_generation` önbelleği tutsun.
  - Kullanıcı başına monoton generation kalıcı olarak saklansın.
  - Aynı `command_id` ile yapılan retry bütçe tüketmesin.
- **Implementation-blocking:** EVET (kullanıcı yetkisi dilimi için; SECURE_APP wire'ını bloke etmez)

### F7 — HIGH (düzeltilmezse BLOCKER) — Anahtarın yeniden verilmesi veya gateway store rollback'i ile nonce reuse

- **Kanıt:** §7.1'de counter 1'den başlıyor ve pe başına tanımlı. §8 "rollback dışlanamıyorsa fail-closed" diyor ama tespit mekanizması yok. Backend pe'sinin rollback'e karşı güvenli olması gerektiği hiçbir yerde yazmıyor (M7P6D bunu yalnızca A2D counter için istiyor).
- **Arıza senaryosu:**
  - Backend DB yedekten dönerse (pe 12→11→12) veya bir hata yüzünden aynı `(tracker, gw, pe, scope, quota)` materyali ikinci kez verilirse, gateway counter'ı 1'den yeniden başlatır ve **aynı anahtar ile aynı nonce** kullanılır.
  - Gateway authority store'u yedekten dönerse veya klonlanırsa sonuç aynıdır.
  - Tracker HWM'i tekrarlanan counter'ı reddettiği için replay güvenli kalır. Ama havada CCM keystream reuse oluşur ve plaintext XOR sızar.
- **Neden önemli:** Denetimin değişmez kuralı açıkça "restored stale storage nonce reuse yapamaz" diyor.
- **Gerekli düzeltme:**
  - (a) Backend `(tracker, gw)` için verilen en yüksek `(pe, gw_gen)` değerini rollback'e karşı güvenli biçimde saklasın ve asla daha düşük veya eşit bir değer yeniden vermesin (M7P6D A2D kuralının eşdeğeri).
  - (b) Gateway, tracker başına şimdiye kadar tuttuğu en yüksek `(pe, gw_gen)` değerinden düşük veya ona eşit materyali reddetsin.
  - (c) Otomatik yedekleme yapan platformlarda (Android/Linux) authority store yedekten hariç tutulsun.
  - (d) Klonlama ve fiziksel imaj geri yükleme, M7P6B tracker'ı için kabul edilen fiziksel risk sınıfı olarak açıkça kaydedilsin.
  - (e) Online olunca gateway son rezerve ettiği counter değerini backend ile karşılaştırsın.
- **Implementation-blocking:** EVET

### F8 — HIGH — Delegated replay durumunun yaşam döngüsü ve reset yolu tanımsız

- **Kanıt:** §9 "ayrı gözden geçirilmiş şema" diyor. Görev metni ayrı bir (sibling) store'u da açık bırakıyor. Taslakta "bozulma → fail-closed" var ama çıkış yolu yok.
- **Arıza senaryosu:** Floor ve slot'lar kendi başına sıfırlanabilen ayrı bir store'da tutulur. Servis "fail-closed'dan çıkmak" için bu store'u siler. Floor 0'a iner ve daha önce kabul edilmiş eski pe'deki frame'ler yeniden geçerli olur. Bu **replay resurrection** demektir.
- **Gerekli düzeltme:** Değişmez kural olarak şu yazılsın: delegated floor ve slot durumu yalnızca **yeni bir credential lifetime** (yeni `credential_id` ve `K_root`) ile birlikte sıfırlanabilir. Bu durum credential ile aynı atomik A/B aktivasyon alanında tutulsun. Öneri: SecurityStore format v3, yeni partition yok (bkz. D).
- **Implementation-blocking:** EVET

### F9 — HIGH — Relay kalıcı custody'si kimliği doğrulanmamış girdi kabul ediyor

- **Kanıt:** §15–16'da "bounded persistent pending-command queue" ve "explicit admission policy" geçiyor ama kabul ölçütü yok. Relay iç frame'i doğrulayamıyor.
- **Arıza senaryosu:** RF saldırganı gerçek tracker ID'lerini (havada açık) hedefleyen, sözdizimi geçerli rastgele GW2D frame'leri basar.
  - Kuyruk ve flash dolar, flash aşınır.
  - Meşru komutlar düşer veya çöp komutların arkasında bekler.
  - Relay çöp frame'leri tracker'ın RX penceresinde yayınlar (her biri 2,1 s).
- **Gerekli düzeltme:**
  - İlk custody RAM-only olsun: target başına 1–2 kayıt, global olarak birkaç kayıt, kalıcılık yok. Reboot kaybı kabul edilsin.
  - Kalıcı custody ya relay'in doğrulayabileceği bir gateway→relay admission MAC'i gelene kadar ertelensin ya da flash yazma bütçesiyle sınırlansın. Bu MAC, enrolled relay ile gateway arasında çift başına anahtar kullanan yeni bir wrapper türü olabilir; relay tracker anahtarı almaz.
  - SECURE_APP freeze'i bu yüzden bloke olmaz.
- **Implementation-blocking:** EVET (kalıcı relay custody dilimi için)

### F10 — MEDIUM — Dedupe tuple zehirlenmesi

- **Kanıt:** §15'te dedupe anahtarı `(target, ctx, origin, gen, counter)`; tag bu anahtarda yok.
- **Arıza senaryosu:** Counter'lar sıralı ve tahmin edilebilir. Saldırgan bir sonraki meşru tuple'ı sahte tag ile önceden gönderir. Relay meşru frame'i "duplicate" sayıp atar.
- **Gerekli düzeltme:** Dedupe tam frame baytları veya `tuple || tag` üzerinden yapılsın. Aynı tuple ile farklı tag taşıyan frame'ler sınırlı sayıda ayrı tutulsun. Tuple'a `key_epoch` de eklensin.
- **Implementation-blocking:** EVET (relay dilimi)

### F11 — MEDIUM — Sıkı HWM altında gateway tarafında serileştirme kuralı yok

- **Kanıt:** M7P6D §5.3 A2D için "bir çıkıştaki frame bitmeden bir sonrakine geçme" kuralını koyuyor. Taslakta GW2D için bunun karşılığı yok.
- **Arıza senaryosu:** Aynı gateway iki komut gönderir (counter 10 ve 11) ve bunlar farklı yollardan gider. 11 önce ulaşır, 10 kalıcı olarak reddedilir. Tracker replay'lere yanıt vermediği için sessiz bir kayıp oluşur.
- **Gerekli düzeltme:** Gateway `(tracker)` başına tek bir çıkıştaki distinct frame tutsun (byte-identical yeniden gönderim serbest). Scope'lar arası sıralama bağımlılığı da dokümana yazılsın.
- **Implementation-blocking:** EVET (gateway gönderici dilimi)

### F12 — MEDIUM — Birden çok custodian'ın aynı anda teslimi çarpışma yaratıyor

- **Kanıt:** §15'te gateway→relay bacağı ve custody'nin kimde olacağı tanımsız.
- **Arıza senaryosu:** Tracker uplink'ini duyan gateway ile iki relay aynı RX penceresinde aynı anda 1,8–2,1 s'lik frame yayar. Çarpışma olur ve hiçbiri teslim edilemez. 100 veya daha fazla düğümde bu yapısal olarak kötüleşir.
- **Gerekli düzeltme:** Tek custodian kuralı (gateway custody relay'ini seçer) ya da deterministik slot/backoff (ör. `hash(relay_id, counter) mod k` × slot).
- **Implementation-blocking:** EVET (relay dilimi)

### F13 — MEDIUM — Kripto öncesi anahtar seçim sırası eksik

- **Kanıt:** §13 ön-kripto red listesinde `key_epoch == current`, `authority_generation ≥ floor`, dolu slot durumunda bilinmeyen gw, `counter ≤ quota_max` ve "reddedilen frame'e RF yanıtı yok" kuralları eksik.
- **Arıza senaryosu:**
  - Tracker anahtarı header'daki `key_epoch` ile türetirse, ileride eklenecek bir epoch rotasyonundan sonra eski epoch frame'leri kabul edilir. Bu resurrection'dır (M7P6D kuralı ile çelişir).
  - Replay'e yanıt veren bir tracker yakalanmış frame'lerle pil DoS'una ve yansıtma saldırısına açık olur.
- **Gerekli düzeltme:** Sıralama G'de verildi; aynen dokümana eklenmeli.
- **Implementation-blocking:** EVET

### F14 — MEDIUM — ConfigStore revision semantiği CAS için yeterli değil

- **Kanıt:** `firmware/include/config_store.h` içinde `generation_` private ve accessor yok. `ConfigStore::recover()` en yüksek geçerli sayfayı seçiyor. Yeni sayfa bozulursa eski sayfaya (N−1) düşülüyor; iki sayfa da bozulursa generation 0 oluyor.
- **Arıza senaryosu:**
  - (1) **ABA:** Rev 13 sayfası bozulur ve 12'ye dönülür. Sonraki kayıt yeniden "13" olur ama içerik farklıdır. "13 bekleyen" gecikmiş bir komut geçer.
  - (2) §12'nin sırası "revision kontrolü önce" diyor. RESULT kaybolduktan sonra aynı `expected_revision` ile gelen retry, `ALREADY_SATISFIED` yerine `STALE_PRECONDITION` alır.
- **Gerekli düzeltme:**
  - Salt-okunur `configRevision()` accessor'ı eklensin.
  - Sıra şu olsun: desired == current ise `ALREADY_SATISFIED`; değilse expected ≠ current ise `STALE`; değilse doğrula ve uygula.
  - Revision token'ı fallback/reset sonrasında asla tekrarlanmasın. Minimal çözüm: ConfigStore format bump ile rastgele 32 bit bir lineage eklemek ve ABA olasılığını ≤2⁻³² yapmak. Alternatif: ABA riskini yazılı olarak kabul etmek.
  - Unchanged, failed ve reset davranışı zaten doğru.
- **Implementation-blocking:** EVET (config yazma dilimi, adım 6)

### F15 — MEDIUM — RESULT korelasyonu ve yönlendirmesi

- **Kanıt:** §14.2 yalnızca `command_id` taşıyor. RESULT anahtarının hangi scope'la seçileceği tanımsız. Taslak, orijin gateway RF erişiminde değilken RESULT'ın nasıl ulaşacağını tanımlamıyor.
- **Arıza senaryosu:** Eski bir `STALE` RESULT, aynı `command_id`'nin sonraki denemesine atfedilir. Ya da RESULT hiçbir zaman orijine ulaşmaz ve UI sonsuza kadar "beklemede" kalır.
- **Gerekli düzeltme:**
  - RESULT içine yanıtladığı GW2D denemesinin `request_counter_be64` değeri eklensin.
  - RESULT, komutla aynı `(pe, gw_gen, scope, quota)` D2GW anahtarıyla şifrelensin.
  - Doğrulayamayan gateway'ler RESULT'ı opaque olarak online olunca backend'e yüklesin; backend D2GW anahtarını türetebilir.
- **Implementation-blocking:** EVET (wire)

### F16 — MEDIUM — Airtime tablosu tek frame'i sayıyor, komut zincirini değil

- **Kanıt:** §18'deki tabloda kare başına sayılar doğru (I'da doğrulandı). Ama komut başına maliyet yalnızca 96 B'lık frame olarak alınmış.
- **Arıza senaryosu:** Relay yolunda gerçek zincir yaklaşık 7,3 s (3,4 kat). 1000 tracker'a tüm fleet için yapılan tek bir config itmesi: gateway bacağı %10 duty cycle altında saatte ≤199 frame gönderebilir, yani en az yaklaşık 5 saat sürer.
- **Gerekli düzeltme:** Zincir tablosu, duty-cycle tavanı ve "fleet push" senaryosu dokümana eklensin.
- **Implementation-blocking:** HAYIR

### F17 — MEDIUM — Gecikmiş store-forward için yasak aileler eksik listelenmiş

- **Kanıt:** §11 yalnızca "non-idempotent/high-risk actuation" ailesini anıyor.
- **Arıza senaryosu:** Saatlerce gecikmiş FREE_GRAZE geofence alarmını kapatır. Gecikmiş RF-config değişikliği tracker'ı erişilemez bırakır. Gecikmiş OPEN_BLE kimse yokken BLE'yi açar.
- **Gerekli düzeltme:** Challenge veya tazelik semantiği tanımlanana kadar şu ailelerin store-forward'u yasaklansın: aktüasyon, FREE_GRAZE/LOST/SEARCH mod geçişleri, RF parametreleri, OPEN_BLE, güvenlik ve credential işlemleri, DFU tetikleme, MESSAGE (TTL tanımlanana kadar).
- **Implementation-blocking:** HAYIR (ilgili aileler için EVET)

### F18 — LOW — Wire ayrıntıları

- `header_len` gereksiz.
- `hop_limit` değerleri tanımsız.
- `(security_context, app_family)` matrisi tanımsız.
- A2D ve D2A için origin, target ve `authority_generation` anlamı tanımsız.
- Aile başına minimum N yok.
- `expected_revision` için "precondition yok" durumu belirtilmemiş.
- `0xFFFF`/`0xFFFFFFFF` epoch değerleri ayrılmamış.
- Düzeltmeler G'de. **Implementation-blocking:** EVET (wire freeze öncesi, küçük düzeltmeler)

### F19 — LOW — command_id semantiği açık yazılmalı

- Tracker `command_id` saklamıyor; idempotency CAS ve durum üzerinden sağlanıyor. "Aynı id, farklı payload" durumunu yalnızca gateway/backend reddedebilir.
- Açıkça yazılmazsa implementer, reboot'ta sıfırlanan RAM tabanlı bir id önbelleği ekleyip sahte bir güvenlik hissi yaratabilir.
- **Implementation-blocking:** HAYIR

### F20 — LOW — 4 slot ve 5. gateway

- Güvenlik açısından doğru (reddediliyor).
- Ancak backend bir `(tracker, floor)` için 4'ten fazla grant verirse, hangi dördünün slot alacağı ilk komut sırasına bağlı ve deterministik değil.
- Düzeltme: Backend grant sayısını ≤4 ile sınırlasın. Aynı `gw_id`'nin iki slotta görünmesi, azalan HWM ve floor'u aşan slot kurtarmada FAULT sayılsın.
- **Implementation-blocking:** HAYIR

### F21 — LOW — ConfigStore aşınması ve API

- Her kayıt bir 4 KiB erase yapıyor. Günde 10 kayıtta sayfa başına yılda yaklaşık 1.825 erase oluşur; nRF52840 tipik endurance'ı (yaklaşık 10k) bu hızla yaklaşık 5,5 yılda tükenir.
- Tracker'da uptime tabanlı bir yazma hız limiti gerekli.
- `requestSave()` hem no-op hem async başlangıç için `true` döndürüyor. Çağıran ya önceden karşılaştırmalı ya da API bir enum döndürmeli.
- **Implementation-blocking:** HAYIR

### F22 — LOW — Görünür metadata

- scope, quota, gateway, tracker ve counter açıkta. Trafik analiziyle komut sınıfı ve sıklığı anlaşılabiliyor.
- Kabul edilebilir, ama dokümanda yazılı olmalı.
- **Implementation-blocking:** HAYIR

### F23 — LOW — Doküman uzlaşması

- M7P6D ve Protocol Evolution Plan'da "gateway counter mint etmez / anahtar sahibi olmaz" ifadeleri geçiyor. Bunlar delegated bağlam istisnasıyla uzlaştırılmalı.
- KDF artık `gateway_device_id` (64 bit DeviceIdentity) içeriyor. Bu, M7P6D'nin kimliği KDF dışında tutma kararından bilinçli bir sapma ve öyle kaydedilmeli.
- **Implementation-blocking:** HAYIR

### F24 — INFO — Backend anahtar saklama bağımlılığı

- HKDF-Expand için backend'in her tracker'ın PRK'sine (veya K_root'una) erişimi gerekiyor. Bu zaten A2D/D2A için de şart.
- Bu gereksinim, PRK'nin HSM'de export edilemeyen bir HMAC anahtarı olarak durduğu bir modelle karşılanabilir.
- Ancak custody mimarisi **UNKNOWN / onaylanmamış**; bağımlılık açıkça adlandırılmalı.

### F25 — INFO — Gateway platformu ve partition bilinmiyor

- 1000 tracker için gereken materyal flash'ta yaklaşık 80–100 KB (bkz. J).
- Bunun için ayrılmış bir partition yok ve gateway platformu **UNKNOWN**.
- ADR persistence layout değişikliği gerekecek.

## C. Kripto / nonce analizi

**Delegated HKDF:** Sağlam.

- info injective, çünkü sabit etiket ve sabit uzunlukta alanlar kullanılıyor (14+1+4+8+4+1+1 = 33 B).
- M7P6D etiketi `"ORUN-TLP-V2-AEAD"` (21 B) ile 12. baytta çakışmıyor ('A' ≠ 'G'). Uzunlukları da farklı.
- RFC 5869, aynı PRK'nin farklı info değerleriyle çoklu türetmede kullanılmasını açıkça destekliyor.
- Tracker kimliği KDF'de yok, ama PRK tracker'a özgü olduğu için örtük olarak bağlı.
- `credential_id` salt olarak lifetime'ı bağlıyor.
- Eksikler: uzunluk öneki yok (sabit alan kuralı yazılmalı), `gw_gen` yok (F3), etiket kayıt defteri yok.
- Downgrade: bilinmeyen scope/quota ya da ayrılmış bitler AEAD öncesi reddedilmeli.

**Anahtar ayrımı:**

- Yön (0x03/0x04), `key_epoch`, gw, pe, scope ve quota değerlerinin hepsi farklı anahtar üretir.
- Scope ayrımı ancak F5 düzeltilirse gerçek olur.

**Scope anahtarları arasında paylaşılan sender counter:**

- Nonce benzersizliği açısından **güvenli**. Farklı anahtarlar ve ortak benzersiz counter, `(key, nonce)` çiftinin benzersizliğini fazlasıyla sağlıyor.
- Replay tarafında scope'lar arası sıralama bağımlılığı (F11) ve quota belirsizliği (F2) var. Yetki bağımlılığı yok, çünkü aynı gateway söz konusu.
- Scope bazlı iptal ancak bir nesil ilerlemesiyle yapılabiliyor.

**Nonce benzersizliği:**

- GW2D: `epoch||0x03||gw_ctr`. Koşullar:
  - (i) tek allocator;
  - (ii) kullanımdan önce kalıcı ve readback ile doğrulanmış rezervasyon;
  - (iii) `(tracker, gw, pe, gw_gen, scope, quota)` materyalinin asla yeniden verilmemesi (F7).
  - (iii) sağlanmazsa **nonce reuse** oluşur.
- D2GW: `epoch||0x04||device_tx_ctr`. Mevcut `SecurityStore::reserveNextTxCounter()` tek allocator olduğu için D2A ile paylaşım **güvenli**. Farklı anahtar zaten yeterli, benzersiz counter ek bir güvence.

**Reserve-ahead (64):**

- Güvenlik doğruluğu açısından blok boyutu önemsiz; kural "persist ve verify, sonra kullan". Yazma ile gönderim arasında veya gönderim ile kayıt arasında crash olması güvenli, çünkü burn ediliyor.
- Kullanılabilirlik açısından 64, 15/63'lük quota ile **uyumsuz** (F1).
- Öneri: blok = 1. Komut seyrek; 1000 tracker × 1 komut/gün yaklaşık 1000 append/gün eder, halka yapıda aşınma ihmal edilebilir.

**Gateway klonlama / rollback:**

- Taslağın "fail-closed" iddiasının tespit mekanizması yok.
- Tracker HWM'i replay'i durdurur ama havadaki nonce reuse'u durduramaz.
- Klonlayan saldırgan anahtara zaten sahip olduğu için ek bir forgery kazancı olmaz; asıl risk **kazara geri yükleme** ve **backend rollback'i**.
- F7 düzeltmeleriyle kabul edilebilir bir risk sınıfına iner.

## D. İptal / epoch / replay analizi

**gateway_policy_epoch:**

- Tanımlandığı biçimde **güvenli ama işletme açısından hatalı**.
- Senaryo 1 (A pe 10, B eklenmiş pe 11, tracker 11'i duymamış): A'nın pe-10 komutunun kabulü kasıtlı ve doğru. Ancak B'nin ilk komutu A'yı öldürür (F3).
- Senaryo 2: Maruziyet = kalan quota × A'nın elindeki scope'lar × A'nın materyal tuttuğu tüm tracker'lar. Zaman sınırı yok (F4).
- Senaryo 3 (floor kalıcılaştırılırken güç kesintisi): Floor ve yeni slot tek bir commit-last kayıtta ise ve eski slotlar floor commit'inden önce silinmiyorsa **güvenli**. Kesinti commit'ten önceyse floor 11'de kalır, eski slotlar sağlamdır ve dispatch olmamıştır. Commit'ten sonraysa 11 reddedilir.
- Senaryo 4: Yüksek epoch'un önce gelmesi, daha düşük epoch'lu bekleyen komutları öldürür (kullanılabilirlik DoS'u, F3/F11).
- 0xFFFFFFFF ayrılmalı.

**Stale-revoke penceresi:**

- Sayı olarak quota ile sınırlı, zaman olarak sınırsız.
- Aktif floor itme mekanizması şart (F4).

**Dört slotlu model:**

- Güvenlik açısından doğru: sessiz eviction yok, doğrulanmamış girdi slot ayıramıyor.
- Slot DoS yapmak için backend'in 4'ten fazla grant vermesi gerekir (F20).
- Aynı `gw_id`'nin iki slotta bulunması, azalan HWM veya floor ≠ slot pe durumları kurtarmada FAULT sayılmalı.

**Güvenli slot geri kazanımı:**

- Geri kazanım yalnızca floor ilerlediğinde yapılır ve floor değişmez biçimde düşük epoch'u reddeder. Bu yüzden meşru geri kazanım eski bir komutu yeniden geçerli **kılamaz**.
- Tek istisna, F8'deki reset yolu. Bu durumun sıfırlanmasına ancak credential lifetime ile birlikte izin verilirse sorun kapanır.
- Konum: ayrı bir store değil, **SecurityStore format v3**. Gerekçeler:
  - atomiklik ve credential'a bağlılık;
  - boş partition yok;
  - v2'nin 40 B'lık kaydının 8 B `value` alanı `(gw_id, gen, bound)` üçlüsünü taşıyamaması, bu nedenle yeni kayıt tipi ve format bump gerekmesi.
- Compaction snapshot'ı TX, A2D, floor ve 4 slotu, yani en çok 7 kaydı taşır.

**Quota modeli:**

- "Maksimum counter" semantiği doğru; tracker bunu durumsuz olarak uygulayabilir.
- quota_code'un **hem KDF'de hem AAD'de** bulunması doğru.
- Ancak F1 ve F2 düzeltilmeden quota tutarlı bir güvenlik sınırı değil.
- Kötü niyetli bir gateway counter atlatarak yalnızca kendi quota'sını yakabilir. Ürettiği flash aşınması da quota/8 append ile sınırlı; bu da iyi bir özellik.

## E. Offline kullanıcı yetkisi analizi

- Model (backend imzalı, gateway'den bağımsız, tracker'a gitmeyen, gateway başına bütçe) **doğru yönde**.
- Kullanıcı kimliği LoRa'ya çıkmıyor ve BLE/PIN yetki olarak kullanılmıyor.
- Ancak şu haliyle **ilerlemeye yeterli değil** (F6). Grant:
  - bir bearer token;
  - hedef kümesine ve tenant'a bağlı değil;
  - online iptal kontrolü içermiyor;
  - generation rollback'ine karşı korunmuyor.
- Ele alınan saldırılar:
  - Aynı gateway'de grant replay'i yalnızca bütçe tüketir.
  - Kopyalama ve telefon hırsızlığı PoP ile sınırlanır; Keystore ele geçirilirse risk kalır.
  - Gateway'lerin toplam bütçesi = Σ(gateway bütçeleri) olarak açıkça yazılmalı.
  - Gateway factory reset'i bütçeyi sıfırlar, ancak re-enroll Internet gerektirdiği için o anda iptal durumu çekilmeli.
  - Bütçe store'unun rollback'i F7 ile aynı risk sınıfında.
  - Backend doğrulama anahtarının rotasyonu için key-id ve enrollment ile gelen trust anchor güncellemesi gerekli.
  - İmza şeması seçilmemiş ve gateway'de verify yolu fiziksel olarak **UNKNOWN**.
- Sonuç: Bütçe sınırlı artık risk, F6 düzeltmeleriyle **tutarlı** hale gelir. Farklı bir ispat modeline gerek yok.

## F. Komut tazeliği / idempotency analizi

- **CAS:** Gecikmiş C komutu (rev 12 → tracker 13) `STALE_PRECONDITION` alır. Doğru; ABA (F14) istisnası dışında.
- **Kayıp RESULT → retry:** Sıra düzeltilirse `ALREADY_SATISFIED` döner ve ikinci flash yazması olmaz, çünkü ConfigStore'un unchanged yolu zaten yazmıyor.
- **Replay commit sonrası, save öncesi güç kesintisi:** Komut kaybolur ve RESULT gitmez. Yeni counter ile retry uygulanır. Güvenli.
- **Save sonrası, RESULT öncesi kesinti:** Retry `ALREADY_SATISFIED` alır. Güvenli.
- **Save sırasında torn yazma:** Eski sayfa aktif kalır. Güvenli.
- **Kalıcı idempotency journal gerekli mi?** Tam kayıt, desired-state config yazımı için **gerekmiyor**. CAS ile sonuç durumu yeterli.
  - Artık belirsizlik: C1 uygulanıp sonra C2 tarafından üzerine yazılmışsa, C1'in retry'ı `STALE` alır ve "hiç uygulanmadı" ile ayırt edilemez. Bu, sahte başarı değil, dürüst bir belirsizlik; `resulting_revision` ile raporlanmalı.
  - Kısmi alan yamaları ve çok parçalı config'ler (geofence poligonları gibi) bu modele sığmaz; ayrı tasarım gerektirir.
- **ConfigStore'un mevcut durumu:**
  - Accessor yok.
  - Blank/default durumu gen=0 ve `hasCommittedRecord=false`.
  - Reset gen'i artırıyor (iyi).
  - Failed ve unchanged kayıtlar gen'i artırmıyor (iyi).
  - Corruption fallback gen'i tekrarlıyor (kötü).
  - Gelecekteki migration monoton gen'i taşımalı.
  - Minimal değişiklik: accessor ekle, token'ın tekrarlanmamasını sağla, CAS sırasını düzelt.
- **Store-forward yasaklı aileler:** F17'deki liste.

## G. Wire layout incelemesi

Doğrulananlar:

- Big-endian kullanımı ve sınırlı uzunluk: 48+N, N≤32.
- `uint8 + 48` işlemi `int`'e terfi eder, taşma yok.
- CCM ile 13 B nonce kullanıldığında L=2; 40 B AAD ve 32 B payload limitlerin çok altında.
- 8 B tag ile forgery olasılığı deneme başına 2⁻⁶⁴.
- AAD bayt 0..39'u kapsıyor, dolayısıyla görünür alanların değiştirilmesi tag hatası verir. Değiştirilmiş alanlar farklı anahtar türetmeye de yol açtığı için hata iki yönden garanti.
- v1/v2 ayrımı bayt 0 üzerinden yapılıyor; belirsizlik yok (`firmware/src/network_service.cpp:110` `payload[0] != 1` ise reddediyor).

**Güvenli alım sırası:**

1. Uzunluk, version, type ve ayrılmış bitler.
2. `target == self` ve `(ctx, family)` izinli.
3. `key_epoch == current`.
4. `pe ≥ floor`. pe == floor ise gw slotta olmalı ya da boş slot bulunmalı; `gw_gen ≥ slot.gen`.
5. `counter ≤ quota_max` ve scope/quota kodu geçerli.
6. HKDF.
7. AEAD. Başarısızsa **RF yanıtı yok** ve durum değişikliği yok.
8. Replay: `counter > HWM`.
9. Floor, slot ve HWM atomik ve kalıcı olarak commit edilir.
10. Plaintext parse edilir ve opcode↔scope kontrol edilir.
11. CAS ve uygulama.
12. RESULT. Replay reddinde RESULT gönderilmez.

4. ve 5. adımlar mutasyon yapmayan ön filtrelerdir. Görünür alanlar yalnızca anahtar **seçimi** için kullanılır ve 7. adıma kadar güvenilmez kabul edilir.

**Düzeltilmiş SECURE_APP adayı** (toplam boyut aynı, 48..80):

| off | size | alan |
|---|---|---|
| 0 | 1 | version = 0x02 |
| 1 | 1 | type = 0x01 SECURE_APP |
| 2 | 1 | security_context (01 A2D, 02 D2A, 03 GW2D, 04 D2GW) |
| 3 | 1 | app_family (GW2D→COMMAND, D2GW→RESULT; matris dışı reddedilir) |
| 4 | 1 | path_flags: bit0 = relay_allowed (0 yalnızca direkt, 1 tek hop); bit1..7 = 0 |
| 5 | 1 | grant: quota[1:0], scope[5:2], [7:6] = 0; A2D/D2A için 0 |
| 6 | 1 | ciphertext_len (aile minimumu: COMMAND/RESULT ≥16, ≤32) |
| 7 | 1 | reserved = 0 (`header_len` kaldırıldı) |
| 8 | 8 | origin_device_id (A2D: 0) |
| 16 | 8 | target_device_id (D2A: 0) |
| 24 | 4 | key_epoch (0xFFFFFFFF geçersiz) |
| 28 | 8 | security_counter (≥1) |
| 32 | 2 | gateway_policy_floor pe (A2D/D2A: 0; 0xFFFF geçersiz) |
| 34 | 2 | gateway_grant_gen (A2D/D2A: 0; 0xFFFF geçersiz) |
| 36 | 4 | reserved = 0 (ileride kullanım ya da kaldırılıp header 36 B yapılabilir) |
| 40 | N | ciphertext |
| 40+N | 8 | tag |

Not: 36..39 kaldırılırsa header 36 B olur ve maksimum frame 76 B'ye düşer (1,74 s). İkisi de kabul edilebilir; owner'ın kararına bırakılmalı. KDF info'ya `gw_gen_be16` eklenmeli ve `pe` 16 bit'e indirilmeli.

**COMMAND:**

| off | size | alan |
|---|---|---|
| 0 | 1 | schema=1 |
| 1 | 1 | opcode (scope tablosuna bağlı) |
| 2 | 1 | args_len (= N−16, tam eşleşme) |
| 3 | 1 | flags: bit0 = has_precondition; diğer bitler 0 olmalı |
| 4 | 8 | command_id |
| 12 | 4 | expected_revision token (has_precondition=0 ise 0 olmalı) |
| 16 | ≤16 | args |

**RESULT:**

| off | size | alan |
|---|---|---|
| 0 | 1 | schema=1 |
| 1 | 1 | result_code |
| 2 | 1 | detail_len (≤8, tam eşleşme) |
| 3 | 1 | flags = 0 |
| 4 | 8 | command_id |
| 12 | 8 | request_counter (yanıtlanan GW2D denemesi) |
| 20 | 4 | resulting_revision |
| 24 | ≤8 | detail (iç durum sızdırmamalı) |

RESULT komutla aynı `(pe, gen, scope, quota)` D2GW anahtarıyla şifrelenir. Orijin gateway'e bağlılık KDF'deki gw id üzerinden sağlanır. `TX_DONE`, gateway'in alması ve relay custody'si hiçbir zaman `APPLIED` olarak yorumlanmaz; `APPLIED` yalnızca kimliği doğrulanmış RESULT ile gelir.

**Relay wrapper:**

- Yapı kabul edilebilir.
- Kimlik doğrulaması yok, bu yüzden `relay_id`/RSSI/SNR güvenilmeyen gözlemdir.
- İç frame `version==2 && type==0x01` olmalı. Aksi halde (iç içe wrapper dahil) reddedilir.
- `inner_len == size−16` olmalı.
- `relay_allowed == 1` değilse custody ve teslim yapılmaz.

## H. Relay custody / DoS analizi

- **Kimliği doğrulanmamış kalıcı kabul, taslakta çözülmemiş (F9).**
- Minimum güvenli başlangıç sözleşmesi:
  - RAM-only custody; target başına ≤1–2 kayıt, global ≤8 kayıt.
  - Tam frame üzerinden dedupe (F10).
  - Teslim deneme üst sınırı: hedefin ≤3 uyanma fırsatı.
  - Relay'in kendi monotonik uptime'ına göre saklama tavanı; tracker'ın saatine güvenilmez.
  - Tek custodian ya da deterministik slotlama (F12).
- Silme yalnızca deneme ve süre sayaçlarıyla yapılır. Sahte bir RESULT silme tetikleyemez; en fazla öncelik ipucu olabilir.
- Tamamlanmış komutların yeniden teslimi tracker'da flash'sız bir replay reddi ile biter. Maliyeti deneme tavanıyla sınırlı: en çok 3 × 2,1 s airtime.
- Reboot'ta RAM kaybolur; gateway RF erişimindeyse yeniden gönderir.
- Kalıcı custody, enrolled relay için bir admission MAC'i tasarlanana kadar ertelenmeli.
- Kritik ve normal trafik adaleti: komut teslimi tracker'ın RX penceresindeki canlı veya kritik uplink'i bloke etmemeli.

## I. RF / airtime yeniden hesabı

Varsayımlar ve formül:

- SF11, BW125 → `Tsym = 2¹¹/125000 = 16,384 ms`.
- LDRO=1. SX126x-Arduino kütüphanesi SF11/BW125'te bunu otomatik açıyor (`radio.cpp:811`).
- Parametreler: CR=1 (4/5), explicit header (H=0), CRC=1, preamble 8 (`firmware/include/radio_config.h`).
- `T = (8+4,25)·Tsym + [8 + ceil((8PL − 4SF + 28 + 16 − 20H) / (4(SF−2)))·5]·Tsym`

| frame | B | sembol | ms |
|---|---:|---:|---:|
| v1 TEST | 18 | 28 | 659,456 |
| v1 POSITION | 34 | 48 | **987,136 ✓** |
| v1 RELAY_FWD | 49 | 63 | **1232,896 ✓** |
| v2 COMMAND/RESULT (N=16) | 64 | 83 | 1560,576 |
| v2 COMMAND config (N=24) | 72 | 88 | 1642,496 |
| v2 max iç frame | 80 | 98 | **1806,336 ✓** |
| wrapper(N=24) | 88 | 108 | 1970,176 |
| v2 max wrapper | 96 | 118 | **2134,016 ✓** |

Taslaktaki dört değer **doğru**. Komut başına zincir:

- Direkt (GW→T 80 B + T→GW 64 B): **3,37 s**
- Relay üzerinden (GW→R 80, R→T 96, T→R 64, R→GW 80): **7,31 s** (taslağın saydığı 2,13 s'nin 3,4 katı)

| tracker | 1 komut/gün (relay) | kanal payı | 10 komut/gün | tek saate yığılırsa (1 komut/gün) |
|---:|---:|---:|---:|---:|
| 10 | 73 s | %0,085 | %0,85 | %2 |
| 100 | 731 s | %0,85 | %8,5 | %20 |
| 1000 | 7307 s | %8,5 | %85 (yapısal çöküş) | %203 (imkansız) |

- **Ham kanal payı:** Seyrek komut 10 ve 100 tracker'da sorun değil. 1000 tracker ve tek SF11 kanalında, arka plandaki v1 POSITION trafiği zaten kapasitenin üstünde: 15 dakikalık aralıkta yaklaşık 1,1 Erlang, pure ALOHA verimi ise yaklaşık %18.
- **Çarpışma:** Birden çok custodian'ın aynı anda teslimi (F12), ölçekle birlikte artan en büyük risk.
- **Regülasyon (varsayım, bölgesel politika koda gömülmedi):** 869,4–869,65 MHz bandında verici başına %10 duty cycle, yani saatte 360 s. Bir gateway saatte en fazla yaklaşık 199 × 1,8 s, bir relay yaklaşık 168 × 2,13 s gönderebilir. Tek bir gateway'den 1000 tracker'a fleet push en az yaklaşık 5 saat sürer; relay'in v1 forwarding işi de aynı bütçeyi paylaşır.
- **Uyku rendezvous'u:** Mevcut `kWindowedRxAfterTxMs = 10000` penceresi 2,1 s'lik bir teslim için yeterli. Uplink aralığı 15–60 dakika ya da 6 saat olabilir; bu gecikme ürün açısından kabul edilmeli.
- **Sonuç:** 10 düğümde kabul edilebilir görünen ama 1000 düğümde çöken model "tek SF11 alanı + fleet push". 1000 düğüm çoklu RF alanı gerektirir; bu, taslağın kendisinin de söylediği gibi, bu dilimin kapsamı dışında.

## J. Kaynak ve flash aşınması analizi

**Tracker:**

- RAM:
  - RX kopyası 255 B × 4 zaten mevcut.
  - Plaintext 32 B.
  - HKDF/CCM çalışma alanı birkaç yüz bayt.
  - Slot önbelleği 4 × ~26 B.
  - Türetilmiş anahtar önbelleği isteğe bağlı, 4 × 32 B.
  - Toplam yaklaşık 1 KB'tan az.
- Kod: tahmini birkaç KB, doğrulanmadı (UNKNOWN).
- Kalıcı durum: SecurityStore v3'te floor ve 4 slot kaydı. Snapshot 7 kayıt, v2'deki 99 slottan headroom yaklaşık 90.
- Komut başına yazımlar:
  - replay append (blok 8);
  - ConfigStore'da 1 sayfa erase ve yazma;
  - gerekirse TX reserve.
- ConfigStore aşınması F21'de.
- Reset fırtınası slot başına blok burn'ü demek; M7P6F L2/L4 ile aynı sınıf.
- Boot taraması küçük.

**Gateway:**

- Tracker başına yaklaşık 80–100 B: id, cred ref, epoch, pe/gen, 2×16 B anahtar/scope, quota, counter bound, RESULT HWM. 1000 tracker için scope başına yaklaşık 80–100 KB flash, ek her scope için +32 KB.
- Bunun için **partition yok** (F25).
- RAM'de tutulmamalı; indeksli flash araması yapılmalı.
- Counter için append log ve halka yapı: günde yaklaşık 1000 append, aşınma ihmal edilebilir.
- Boot kurtarma taraması log boyutuyla doğru orantılı.
- Bunlara offline kullanıcı bütçesi, iptal önbelleği ve RESULT korelasyonu eklenir.
- Gateway-bridge taahhüdü nedeniyle BLE sürekli açık; enerji bu dilimin konusu değil.

**Relay:**

- RAM custody için 8 × ~110 B ve digest dedupe için 32 × 12 B.
- Kalıcı kuyruk yok.

**Sahiplik çakışmaları:**

- History, Config, Bond ve Security bölgeleri dolu. Ayrı bir replay store yeni bir partition ya da ADR değişikliği gerektirir; bu nedenle SecurityStore v3 öneriliyor.
- Flash mutasyonu harici SX1262'nin TX'ini bozmaz. Ancak 85 ms'lik erase CPU'yu durdurur, bu yüzden RESULT zamanlaması ölçülmeli (UNKNOWN).

## K. Tehdit senaryosu matrisi

| # | Senaryo | Durum | Gerekçe |
|---|---|---|---|
| 1 | Pasif yakalama + tracker reboot sonrası replay | SAFE | Kalıcı HWM ve reserve burn |
| 2 | Gateway factory reset / re-enroll sonrası replay | SAFE | Yeni nesil ile yeni anahtar. F7 reissue kuralı şart |
| 3 | Eski tracker security imajının geri yüklenmesi | UNRESOLVED | Fiziksel rollback; M7P6B ile aynı kabul edilen risk, tespit yok |
| 4 | Eski gateway authority yedeğinin geri yüklenmesi | UNSAFE | Nonce reuse (F7) |
| 5 | Gateway klonu | UNSAFE → yalnızca kazara klonda kabul edilen risk | F7(d); tracker HWM replay'i durdurur |
| 6 | Tek gateway ele geçirilmesi | UNRESOLVED | Etki: quota × tüm tracker'lar, zaman sınırı yok (F4) |
| 7 | Tek tracker ele geçirilmesi | SAFE | Etki yalnızca o tracker; RESULT sahteciliği yalnızca kendisi için |
| 8 | OfflineUserGrant'in ele geçirilmesi | UNSAFE | Bearer token, tenant bağı yok (F6) |
| 9 | Kötü niyetli sıradan relay | SAFE (güvenlik) / kullanılabilirlik riski | Drop, delay ve reorder mümkün; replay reddedilir; tazelik F17 |
| 10 | Sahte v2 frame seli | Tracker SAFE; relay UNSAFE | Kalıcı kuyruk ve dedupe zehirlenmesi (F9/F10) |
| 11 | Yetkili gateway'in counter atlatması | SAFE | Yalnızca kendi quota'sı; aşınma quota/8 ile sınırlı |
| 12 | Yetkili gateway'in quota'yı bilerek tüketmesi | SAFE (sınırlı) | Kendi slotu; offline kurtarma yok (F1) |
| 13 | Dört slot doluyken 5. gateway | SAFE | Reddedilir; backend sınırı gerekli (F20) |
| 14 | İptal edilmiş gateway eski grant ile offline | UNRESOLVED | Floor itme yok (F4) |
| 15 | Config değiştikten sonra gelen gecikmiş komut | SAFE* | STALE; *ABA istisnası F14 |
| 16 | Replay commit sonrası, save öncesi güç kaybı | SAFE | Komut kaybolur, retry ile uygulanır |
| 17 | Save sonrası, RESULT öncesi güç kaybı | SAFE* | *CAS sırası düzeltilirse ALREADY_SATISFIED |
| 18 | Duplicate RESULT | SAFE* | *request_counter bağlaması ile (F15) |
| 19 | Counter UINT64_MAX yakınında | SAFE | Quota ≤1023 ön filtresi |
| 20 | key_epoch UINT32_MAX yakınında | UNRESOLVED | Rotasyon yok; key_epoch==current kuralı yazılmalı (F13) |
| 21 | gateway_policy_epoch UINT32_MAX yakınında | UNRESOLVED | Değer ayrılmalı; aksi halde re-provision'a kadar kilitlenme |
| 22 | Config revision wrap | SAFE (wrap) / UNRESOLVED (ABA) | 32/64 bit wrap aşınma nedeniyle erişilemez; fallback ABA'sı F14 |
| 23 | command_id çakışması | SAFE* | Rastgele 64 bit: 1000×10/gün×10 yıl için p≈3,6e−5 fleet-wide, tracker başına ≈3,6e−11. *RESULT bağlaması ile |
| 24 | Görünür header alanlarının değiştirilmesi | SAFE | AAD ve KDF'e bağlı; wrapper alanları güvenilmez |
| 25 | Opaque relay'e sahte ama geçerli frame seli | UNSAFE | F9 |

## L. Owner onayı öncesi gereken değişiklikler

1. Quota'yı gateway grant'i başına tek değer yap, reserve bloğunu 1 (veya ≤4) yap, retry'ın counter maliyetini tanımla (F1, F2).
2. İki seviyeli nesil: floor yalnızca kaldırmada ilerlesin; slotta `gw_gen` olsun; boş slot varsa ekleme epoch ilerletmesin. KDF ve wire'ı güncelle (F3).
3. Aktif floor ilerletme yolu (A2D ya da SYNC) ekle, site/hesap scoping'i ve etki alanı beyanını yaz (F4).
4. `opcode → scope` tablosunu zorunlu kıl (F5).
5. OfflineUserGrant için PoP, hedef/tenant bağı, online iptal ve monoton generation ekle (F6).
6. Backend ve gateway için "nesil asla yeniden verilmez / geri alınmaz" kuralını, yedekten hariç tutmayı ve klonun kabul edilen risk olarak kaydını ekle (F7).
7. Delegated replay durumunu credential lifetime'a bağla ve yerini SecurityStore v3 olarak belirle (F8).
8. Tam güvenli alım sırasını ekle: key_epoch, floor, slot, quota; retlerde RF yanıtı yok (F13).
9. RESULT'a `request_counter` ekle, anahtar seçimini tanımla, backend'e upload yolunu tanımla (F15).
10. G'deki wire ayrıntı düzeltmelerini uygula (F18).
11. CAS sırasını, revision token'ın tekrarlanmamasını ve accessor gereksinimini yaz (F14).
12. İlk relay dilimini RAM-only olarak sınırla; tam frame dedupe, tek custodian ve deneme/süre tavanı ekle (F9–F12).

## M. Bu tasarımı bloke etmemesi gereken ertelenmiş işler

- Kalıcı relay custody ve gateway→relay admission MAC'i.
- Scope başına farklı quota.
- Challenge/tazelik gerektiren aileler (aktüasyon, FREE_GRAZE, OPEN_BLE, RF config, MESSAGE).
- Çok parçalı config ve geofence transaction'ları.
- Çoklu RF alanı ve 1000 düğüm topolojisi.
- İmza şeması seçimi ve gateway'de fiziksel verify KAT'ı.
- Backend HSM custody tasarımı (bağımlılık olarak adlandırılması yeterli).
- Doğrulanabilir relay custody makbuzu.
- Metadata gizliliği.
- ConfigStore yazma hız limitinin sayısal değeri.
- Fiziksel sleepy-tracker store-forward testi (§19, adım 9).

## N. Nihai merge önerisi

Doküman **DRAFT olarak kalmalı, düzeltilmeli ve yeniden denetlenmeli**. Şu haliyle owner onayına veya wire freeze'e uygun değil.

- Yeniden denetim şu kalemlere odaklanmalı:
  - L1–L9'un metne işlenmesi;
  - düzeltilmiş wire tablosu;
  - iki seviyeli nesil modeli için bir crash-order tablosu;
  - KDF info baytlarının kesin hali.
- Bunlar kapandıktan sonra L10–L12 ve F16/F17/F19–F23, onay sonrası non-blocking düzenleme olarak ele alınabilir.
- Çekirdek kararlar korunmalı:
  - per-tracker HKDF delegasyonu;
  - K_root'un gateway'e verilmemesi;
  - AEAD sonrası kalıcı HWM;
  - sessiz eviction olmaması;
  - CAS tabanlı idempotency;
  - v1'in dokunulmamış kalması (bu branch'te doğrulandı).

**Kanıt sınırı:**

- Bu denetim yalnızca doküman ve kod okumasına dayanıyor. Host test, build ya da fiziksel ölçüm yapılmadı.
- Airtime değerleri formülle hesaplandı, RF ölçümü değil.
- Duty-cycle değerleri bir varsayım.
- APPROTECT, gateway platformu ve backend custody **UNKNOWN**.
