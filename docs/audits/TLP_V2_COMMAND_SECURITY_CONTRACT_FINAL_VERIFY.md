# Final focused verification — ORUN TLP v2 delegated command design

Status: **FINAL FOCUSED VERIFICATION RESULT — PASS WITH MINOR DOC FIX.**
Documentation-only; no firmware, test, fixture or protocol byte was changed.

- Branch: `design/tlp-v2-command-security-contract`
- Prompt: `docs/audits/TLP_V2_COMMAND_SECURITY_CONTRACT_FINAL_VERIFY_PROMPT.md`
- Content HEAD verified: `10bc3965fd9dd718d7571aac61cdf7c298e465f1`
- Branch HEAD at verification: `ea0ed42218d87e51c4edfc643c0ee3f832372922` (adds only the prompt file on top of `10bc396`)
- Baseline main: `e2a370510c595c5f4b88e94a1212fb95d848a273`
- Primary target: `docs/architecture/ORUN_TLP_V2_COMMAND_SECURITY_CONTRACT_DRAFT_V2.md`
- Audit history:
  - `docs/audits/TLP_V2_COMMAND_SECURITY_CONTRACT_INDEPENDENT_AUDIT.md`
  - `docs/audits/TLP_V2_COMMAND_SECURITY_CONTRACT_REAUDIT.md`
- Verification date: 2026-09-26

Evidence boundary: documentation review only. No host, build, hardware or RF
evidence. CSPRNG, APPROTECT, gateway platform and backend custody are
**UNKNOWN**.

---

## A. Verdict

**PASS WITH MINOR DOC FIX**

- Branch HEAD `ea0ed42` = içerik HEAD `10bc396` + yalnızca prompt dosyası (`git diff --stat 10bc396 HEAD` → 1 dosya).
- Branch'e import edilen re-audit dosyası, `audit/tlp-v2-command-security-contract-reaudit` branch'indeki sürümle byte-identical (`cmp` ile doğrulandı).
- `44ead40..10bc396` aralığında yalnızca doküman değişikliği var; firmware, test, fixture veya protokol kodu değişmedi.

## B. K1–K9 doğrulama tablosu

| # | Madde | Durum | Gerekçe |
|---|---|---|---|
| K1 | N2 — 56 B header yalnızca delegated context için | **CLOSED** | §14 type 0x01 artık `DELEGATED_SECURE_APP` ve yalnızca delegated context kabul ediyor. A2D/D2A ayrı, kompakt bir secure type alacak; routine position/telemetri bu header'ı kullanmıyor. |
| K2 | N4 — Atomik slot tuple, floor ile resurrection yok | **CLOSED** | §9: `(gw_id, gen, replay_bound)` tek commit-last kayıt. Floor, lower-floor slotlar geri kazanılmadan önce durable oluyor. Compaction activation-last. İmkânsız bir kombinasyon FAULT veriyor. Eski HWM'in geri gelme yolu kalmadı. |
| K3 | N5 — TRNG / boot başına reseed / fail-closed | **CLOSED** | §6.2: donanım TRNG veya her boot'ta TRNG'den seed edilen DRBG zorunlu. Kalıcı seed tek başına yasak. RNG health hatasında delegated TX kapanıyor. Tracker RNG coexistence kanıtına dahil. |
| K4 | N1 — HKDF hiyerarşisi | **CLOSED** | `PRK = Extract(credential_id, K_root)` (M7P6D ile aynı). `K_grant = Expand(PRK, grant_info, 32)`. `K_frame = Expand(K_grant, "ORUN-TLP-V2-GW-FRAME-v1" ‖ dir ‖ salt[12], 16)`. Gizli değer her adımda HMAC key konumunda olduğu için yalnızca standart PRF varsayımı gerekiyor. GRANT ve FRAME etiketleri farklı PRK'lar altında, sabit uzunlukta ve injektif. Nonce yapısı (`epoch‖dir‖counter`) değişmedi; anahtar/nonce karışıklığı yok. |
| K5 | N7/F4 — Monoton floor, POLICY_SYNC | **CLOSED** | §4.2: `max(current, received)`. Gecikmiş daha düşük veya eşit güncelleme no-op. POLICY_SYNC normal delegated yoldan geçiyor ve §11 adım 5 (floor < durable ise reddet) nedeniyle floor'u düşüremiyor. |
| K6 | N3 — UNCONFIRMED, relay retention'a karşı sınırlı | **CLOSED** (küçük eksik, D1) | §7.3: timeout `UNCONFIRMED` sayılıyor, `FAILED` değil. Bir sonraki distinct counter, ancak authenticated RESULT gelince veya custody deadline her yerde dolunca üretiliyor. Gateway timeout'unun relay retention + teslim penceresinden kesinlikle büyük olması şartı var. |
| K7 | N6 — Tek yapılandırılmış custodian, Role'den bağımsız | **CLOSED** | §17: `command_custody_enabled` açık bir yapılandırma ayarı. Site/RF domain başına en fazla bir custodian var ve Role'den türetilmiyor. Custodian varsa gateway doğrudan göndermiyor. |
| K8 | F18 — Ayrılmış değerler / counter alt sınırı / aile minimumları | **CLOSED** | §14.2: `counter ≥ 1`; `key_epoch` ve floor için `0xFFFFFFFF` geçersiz; gen için `0` ve `0xFFFFFFFF` geçersiz; her aile kendi minimumunu tanımlıyor (COMMAND için 16). |
| K9 | F23/F24 — Protocol plan istisnası, backend custody | **CLOSED** | `ORUN_PROTOCOL_EVOLUTION_PLAN.md:233` delegated gateway istisnasını içeriyor. §20'de backend PRK/HSM custody'si UNKNOWN bir önkoşul olarak açıkça yazılı ve raw `K_root` export reddediliyor. |

Fix'lerle eklenen iki detay:

- **PoP digest:** **CLOSED.** §10'da imza girdisi `challenge ‖ command_digest(target, opcode, args, command_id)`.
- **Exposure bound:** **KISMEN.** "Kalan per-tracker grant quota'sının toplamı; scope sayısıyla çarpılmaz" ifadesi doğru. Ancak birden fazla generation tutulan durumu kapsamıyor (bkz. D2).

## C. Yeni BLOCKER/HIGH bulgular

NONE

## D. Küçük doküman düzeltmeleri

1. **§7.3 (N3), gateway reboot ve saat farkı:** Gateway reboot olursa bekleyen frame'in deadline'ı en baştan, tam süreyle yeniden başlamalı; uptime tabanlı bir sayaç reboot'ta sıfırlanıp erken bir N+1 üretilmesine yol açmamalı. Timeout eşitsizliğine cihazlar arası saat kayması için bir güvenlik marjı eklenmeli.
2. **§4.2, generation'a göre exposure:** Ele geçirilmiş bir gateway geçmiş grant generation'larının materyalini de saklayabilir. Tracker'ın slotu hâlâ `gen = g` iken saldırgan önce `g`'nin kalan quota'sını, ardından elindeki daha yüksek `g+1`'in tam quota'sını kullanabilir.
   - Doğru sınır: tracker'ın bildiği slot generation'ına eşit veya ondan yüksek olan, gateway'e verilmiş tüm generation'ların kalan quota toplamı. Scope çarpanı yine yok.
   - Buna göre quota yenilemesi, iptal maruziyeti açısından eski quota'nın yerine geçmiyor, ona **ekleniyor**. Bu dokümana yazılmalı.
3. **İsimlendirme:** §18'deki relay wrapper (`16 N exact unchanged SECURE_APP`) ve §22 adım 3 hâlâ eski `SECURE_APP` adını kullanıyor. Bunlar `DELEGATED_SECURE_APP` olmalı.
4. **Wrapper'ın kabul ettiği iç type'lar:** İleride kompakt A2D/D2A type'ları eklenecek. Wrapper'ın hangi iç type'ları taşıyacağı açık bir allow-list ile belirtilmeli; bu ilk dilimde yalnızca type 0x01.

## E. Owner-onayı önerisi

D1–D4 küçük düzeltmeleri yapıldıktan sonra (yeni bir denetim gerekmiyor), şu iki başlık **design direction olarak owner onayına hazır**:

- **Delegated authority mimarisi:**
  - per-tracker HKDF delegasyonu;
  - iki seviyeli floor/generation modeli;
  - ortak quota;
  - opcode→scope zorunluluğu;
  - SecurityStore v3 yaşam döngüsü;
  - OfflineUserGrant modeli;
  - RAM-only tek custodian.
- **56 baytlık delegated secure-frame header'ı ve AAD.**

Bu onay wire freeze ya da implementasyon izni değildir:

- COMMAND/RESULT plaintext'i, config state-token/CAS dilimine kadar **provisional** kalıyor.
- A2D/D2A'nın kompakt type'ları ayrıca incelenecek.
- Bundan sonrası V2 §22'deki dilim sırasına ve orada tanımlı host/RAK KAT, coexistence ve fault-injection kapılarına bağlı.

Bu yalnızca bir doküman incelemesi. Host, build, donanım veya RF kanıtı yok; CSPRNG, APPROTECT, gateway platformu ve backend custody UNKNOWN.
