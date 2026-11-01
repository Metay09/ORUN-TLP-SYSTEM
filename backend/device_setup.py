"""1d/1e: bir tasmayı hesaba (çiftliğe) ekle — uygulamadan, PC aracı olmadan.

Tasma telefona USB ile bağlıdır. Uygulama tasmanın kendi raporunu (BENCH?)
gönderir; sunucu tasmaya yazılacak kurulum satırlarını verir: çiftliğin her
gateway'i için bir satır (tasma en fazla 4 gateway izni tutar). Anahtar
tasma+gateway+nesil başına kalır; ortak çiftlik anahtarı yoktur.

Nesil: tasma her yeni izinde "en büyük mevcut nesil + 1" verir
(SecurityStore::beginV3PolicyMutation). Sunucu bunu kayıtlı izinlerden ve
tasmanın bildirdiği ilk izinden hesaplar; onaylarken tasmanın raporuyla
karşılaştırır. Yanlış nesil veri kaybettirmez: gateway onayı tasmada
doğrulanmaz, kayıtlar tasmada kalır.
"""
from __future__ import annotations

import re

from . import orun_backend_bench as _bench  # noqa: F401  (same credentials module)
from .orun_backend_bench import oc

HEX16 = re.compile(r"^[0-9A-F]{16}$")
HEX32 = re.compile(r"^[0-9A-F]{32}$")
MAX_GATEWAYS = 4   # security_v3_format::kGatewayCount


class SetupRefused(ValueError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


def _report(body: object) -> dict:
    """Validated subset of the Tracker's own BENCH? report."""
    if not isinstance(body, dict):
        raise SetupRefused("invalid_request", "Geçersiz istek.")
    dev, cred, granted = body.get("device"), body.get("credential"), body.get("granted")
    gen, floor, epoch = body.get("generation"), body.get("floor"), body.get("epoch")
    if (not isinstance(dev, str) or not HEX16.match(dev) or
            not isinstance(cred, str) or not (cred == "none" or HEX32.match(cred)) or
            not isinstance(granted, str) or not (granted == "none" or HEX16.match(granted)) or
            type(gen) is not int or type(floor) is not int or type(epoch) is not int or
            not 0 <= gen < 2**32 or not 0 <= floor < 2**32 or not 0 <= epoch < 2**32):
        raise SetupRefused("invalid_request", "Tasma raporu okunamadı.")
    return {"device": int(dev, 16), "credential": cred, "epoch": epoch,
            "granted": None if granted == "none" else int(granted, 16),
            "generation": gen, "floor": floor}


def _plan(registry, rep: dict, cred) -> list[tuple[int, int]]:
    """[(gateway, generation)] the Tracker will hold after the new grants."""
    gateways = registry.farm_gateways()[:MAX_GATEWAYS]
    if not gateways:
        raise SetupRefused("no_gateway",
                           "Çiftlikte henüz gateway yok: önce gateway'i köprüye (PC Edge) bağla.")
    known = {g: n for g, (_f, n) in registry.tracker_grants(cred.device_id, cred.key_epoch).items()}
    if rep["granted"] is not None and rep["generation"]:
        known.setdefault(rep["granted"], rep["generation"])
    plan = []
    for g in gateways:
        if g not in known:
            known[g] = max(known.values(), default=0) + 1
            plan.append((g, known[g]))
    return plan


def prepare_tracker(registry, body: object) -> dict:
    rep = _report(body)
    dev = rep["device"]
    cred = registry.active(dev)
    if rep["credential"] == "none":
        if cred is None:
            cred = registry.create(dev)
    elif cred is None or rep["credential"] != cred.credential_id.hex().upper():
        raise SetupRefused("foreign_credential",
                           "Tasmada başka bir anahtar var; önce temizleme imajıyla sıfırlanmalı.")
    if rep["credential"] != "none" and rep["epoch"] != cred.key_epoch:
        raise SetupRefused("foreign_credential", "Tasmadaki anahtar dönemi kayıtla uyuşmuyor.")
    plan = _plan(registry, rep, cred)
    return {
        "device": f"{dev:016X}", "epoch": cred.key_epoch,
        "plan": [{"gateway": f"{g:016X}", "generation": n} for g, n in plan],
        # Kurulum satırı tasmanın kök anahtarını taşır: yalnız bu tasmaya,
        # hesabın sahibi üzerinden, USB'ye yazılmak için verilir; saklanmaz.
        "lines": [oc.provision_line(cred, g) for g, _n in plan],
    }


def confirm_tracker(registry, body: object) -> dict:
    """After the lines were written: record the grants the Tracker now holds."""
    rep = _report(body)
    dev = rep["device"]
    cred = registry.active(dev)
    if (cred is None or rep["credential"] != cred.credential_id.hex().upper() or
            rep["epoch"] != cred.key_epoch or rep["granted"] is None):
        raise SetupRefused("not_provisioned", "Tasma kurulumu doğrulanamadı.")
    recorded = registry.tracker_grants(dev, cred.key_epoch)
    first = recorded.get(rep["granted"])
    plan = _plan(registry, {**rep, "granted": None, "generation": 0}, cred) if not recorded else \
        _plan(registry, rep, cred)
    expected_first = first[1] if first else dict(plan).get(rep["granted"])
    if expected_first != rep["generation"]:
        raise SetupRefused("generation_mismatch",
                           "Tasmanın izin durumu beklenenden farklı; PC'de orun_provision.py ile kur.")
    for g, n in plan:
        registry.record_grant(dev, cred.key_epoch, g, rep["floor"], n)
    gateways = sorted(registry.tracker_grants(dev, cred.key_epoch))
    return {"device": f"{dev:016X}", "gateways": [f"{g:016X}" for g in gateways],
            "added": len(plan)}
