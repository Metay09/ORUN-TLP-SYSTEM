# Final focused verification — ORUN TLP v2 delegated command design

Repository: `Metay09/ORUN-TLP-SYSTEM`

Branch: `design/tlp-v2-command-security-contract`

Content HEAD to verify:
`10bc3965fd9dd718d7571aac61cdf7c298e465f1`

Baseline main:
`e2a370510c595c5f4b88e94a1212fb95d848a273`

Primary target:
`docs/architecture/ORUN_TLP_V2_COMMAND_SECURITY_CONTRACT_DRAFT_V2.md`

Audit history:

- `docs/audits/TLP_V2_COMMAND_SECURITY_CONTRACT_INDEPENDENT_AUDIT.md`
- `docs/audits/TLP_V2_COMMAND_SECURITY_CONTRACT_REAUDIT.md`

This is a narrow **post-fix verification**, not another broad redesign audit.
Do not write firmware, change tests, commit, push or open a PR.

First verify that the branch content being reviewed is the same as
`10bc3965...` except for this prompt file if the prompt itself is committed on
top.

Read `AGENTS.md`, the product/system architecture, V2 design, and both audits.

Check only whether the re-audit's requested K1-K9 corrections were applied
correctly and whether those corrections introduced a new BLOCKER/HIGH issue.

Required checks:

1. N2 — the 56-byte/96-byte secure frame is explicitly delegated-command-only,
   not the mandatory header for routine position/telemetry.
2. N4 — `(gateway_id, grant_generation, replay_bound)` is atomic commit-last
   state and floor advancement cannot resurrect old slot/HWM state.
3. N5 — frame diversification randomness requires hardware TRNG or a DRBG
   freshly seeded/reseeded from TRNG each boot; persistent seed alone is
   forbidden; RNG failure is fail-closed.
4. N1 — exact HKDF hierarchy is coherent:
   - credential PRK = M7P6D Extract(credential_id, K_root);
   - K_grant = Expand(PRK, exact grant_info, 32);
   - K_frame = Expand(K_grant,
     frame-label || direction || 96-bit public frame diversification value, 16).
   Confirm this is a standards-based sound use and does not create key/nonce
   confusion.
5. N7/F4 — floor update is monotonic `max(current, X)`, delayed lower updates
   are no-op, and POLICY_SYNC/backend floor advance semantics cannot lower the
   floor.
6. N3 — transport timeout is UNCONFIRMED rather than FAILED and the gateway does
   not mint the next distinct frame while the previous frame may still be
   legitimately delivered by configured relay custody.
7. N6 — first relay slice has one explicitly configured custody-enabled node and
   does not infer custody from legacy Role.
8. F18 — reserved values/counter lower bound/family minimum rules are explicit.
9. F23/F24 — protocol plan contains the delegated-gateway exception; backend/HSM
   credential-derivation custody remains an explicit UNKNOWN prerequisite.

Also verify these two details introduced during the fixes:

- OfflineUserGrant proof-of-possession binds the fresh challenge to the logical
  command digest.
- The residual revoked-gateway exposure is counted as remaining per-tracker
  grant quota; do not multiply it again by scope count because quota is shared
  across scopes.

Output only:

## A. Verdict
PASS / PASS WITH MINOR DOC FIX / FAIL

## B. K1-K9 verification table
One row each: CLOSED / PARTIAL / OPEN, reason.

## C. New BLOCKER/HIGH findings
If none, write NONE.

## D. Minor document corrections
Only concrete remaining corrections.

## E. Owner-approval recommendation
State whether the delegated authority architecture and delegated secure-frame
header are ready for owner approval as design direction.

Do not declare COMMAND/RESULT plaintext wire-frozen: config state-token/CAS
semantics remain intentionally separate and provisional.

Evidence discipline: documentation review is not host/build/hardware/RF proof.
