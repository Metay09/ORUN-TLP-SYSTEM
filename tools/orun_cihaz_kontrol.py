#!/usr/bin/env python3
"""Y1: USB'deki ORUN cihazının yazılım yüklemeden önce ve sonra durumu.

  python3 tools/orun_cihaz_kontrol.py oku --port P --out once.json
  python3 tools/orun_cihaz_kontrol.py karsilastir once.json sonra.json
  python3 tools/orun_cihaz_kontrol.py rol --port P tasma|gateway   # boş cihaz (Y2a)
  python3 tools/orun_cihaz_kontrol.py pil --port P [--izle 60]     # pil ölçümü (Pil 1)

"alan" alan (geofence) belleğinin iki sayfasını yalnız okur ve durumunu söyler;
"alan --hazirla" yalnız kayıtlı alan yokken (önceki yazılım artığı) bölgeyi siler.
"oku" cihaza yalnız okuma soruları sorar (BENCH?, PRODUCT STORE?, APP DEVICE?);
hiçbir şey yazmaz, silmez. Anahtarın kendisi hiçbir zaman okunmaz ya da
yazılmaz: yalnız kimliği (credential id) ve dönemi (epoch) karşılaştırılır.
"karsilastir" yükleme sonrası cihazın aynı cihaz olduğunu, anahtarının ve
kayıtlarının yerinde durduğunu doğrular.
"rol" yalnız kurulmamış (boş) bir cihaza ne olacağını söyler (APP SERVICES T ya
da A); cihaz kaydeder ve o rolde yeniden başlar. Kurulmuş cihazın rolü
değişmez.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import time

TRACKER = re.compile(r"BENCH TRACKER id=([0-9A-F]{16}) .*?test_credential=(yes|no)"
                     r".*?granted_gateway=([0-9A-F]{16}|none).*?product_store=(\w+)")
CREDENTIAL = re.compile(r"BENCH CREDENTIAL credential=([0-9A-F]{32}|none) epoch=([0-9A-F]{8}) "
                        r"grant_generation=([0-9A-F]{8}) custody_floor=([0-9A-F]{8})")
GATEWAY = re.compile(r"BENCH GATEWAY id=([0-9A-F]{16}) custody_region=(\w+)"
                     r"(?: custody_ready=(\w+) held=(-?\d+)/(\d+))?")
OGK1 = re.compile(r"BENCH OGK1 status=(\w+) generation=([0-9A-F]{16})")
STORE = re.compile(r"PRODUCT STORE ready=(\w+) faulted=(\w+) inc=(\w+) next_seq=(\d+) retained=(-?\d+)")
DEVICE = re.compile(r"APP DEVICE id=\d+ fw=(\S+)")
# Y2a tek yazılım: BENCH? yanıtının ilk satırı.
ROLE = re.compile(r"BENCH DEVICE id=([0-9A-F]{16}) role=(\w+) security_region=(\S+) gnss=(\w+)")
ROLE_KIND = {"TRACKER": "tracker", "GATEWAY": "gateway", "UNASSIGNED": "unassigned",
             "CONFLICT": "conflict"}
KIND_WORD = {"tracker": "Tasma", "gateway": "Gateway", "unassigned": "Kurulmamış cihaz",
             "conflict": "Rolü çelişkili cihaz"}


def parse(lines: list[str]) -> dict:
    """What the device told about itself; only public facts, never a key."""
    out: dict = {"kind": None}
    for line in lines:
        line = line.strip()
        if m := TRACKER.search(line):
            out.update(kind="tracker", id=m[1], test_key=m[2] == "yes", granted=m[3],
                       product_store=m[4])
        elif m := CREDENTIAL.search(line):
            out.update(credential=m[1], epoch=m[2], grant_generation=m[3], custody_floor=m[4])
        elif m := GATEWAY.search(line):
            out.update(kind="gateway", id=m[1], custody_region=m[2])
            if m[4] is not None:
                out.update(custody_ready=m[3], held=int(m[4]), capacity=int(m[5]))
        elif m := OGK1.search(line):
            out.update(ogk1=m[1], ogk1_generation=m[2])
        elif m := STORE.search(line):
            out.update(store_ready=m[1], store_faulted=m[2], store_inc=m[3],
                       next_seq=int(m[4]), retained=int(m[5]))
        elif m := DEVICE.search(line):
            out.update(fw=m[1])
        elif m := ROLE.search(line):
            out.update(role=m[2], gnss=m[4])
            if out["kind"] is None:
                out.update(kind=ROLE_KIND.get(m[2], m[2].lower()), id=m[1])
    return out


def complete(info: dict) -> bool:
    """Did the device tell everything the comparison needs?

    Right after a restart the tracker's record store may still be busy for a
    few seconds; it then prints next_seq=0 ("not known yet"), which is not a
    real sequence. Such a reading is asked again rather than compared.
    """
    if not info.get("kind"):
        return False
    if info["kind"] == "tracker" and info.get("store_inc") and info.get("next_seq", 0) == 0:
        return False
    return True


def compare(before: dict, after: dict) -> list[tuple[bool, str]]:
    """(ok, Turkish line) for each thing that must have survived the update."""
    res: list[tuple[bool, str]] = []

    def need(ok: bool, text: str) -> None:
        res.append((bool(ok), text))

    need(after.get("kind") == before.get("kind"), f"Cihaz türü aynı ({after.get('kind')})"
         + ("" if after.get("kind") == before.get("kind") else f" — önce {before.get('kind')}"))
    need(after.get("id") == before.get("id"), f"Cihaz kimliği aynı ({after.get('id')})")
    if before.get("kind") == "tracker":
        need(after.get("credential") == before.get("credential") and after.get("epoch") == before.get("epoch"),
             "Tasmanın anahtarı yerinde (kimlik ve dönem aynı)")
        need(after.get("granted") == before.get("granted") and
             after.get("grant_generation") == before.get("grant_generation"),
             "Gateway izni yerinde")
        if before.get("store_inc"):
            need(after.get("store_inc") == before.get("store_inc"), "Kayıt hafızası aynı (silinmedi)")
            need(after.get("store_ready") == "yes" and after.get("store_faulted") == "no", "Kayıt hafızası hazır")
            b, a_ = before.get("next_seq", 0), after.get("next_seq", 0)
            if a_ == 0:
                need(False, f"Kayıt sırası okunamadı (hafıza meşgul; önce {b})")
            else:
                need(a_ >= b, f"Kayıt sırası geri gitmedi ({b} → {a_})")
    elif before.get("kind") == "gateway":
        need(after.get("custody_region") == "READY" or after.get("custody_region") == before.get("custody_region"),
             f"Gateway belleği hazır ({after.get('custody_region')})")
        if before.get("held") is not None:
            need(after.get("held", -1) >= before.get("held", 0),
                 f"Gateway'deki kayıtlar yerinde ({after.get('held')}/{after.get('capacity')})")
        if before.get("ogk1_generation"):
            need(after.get("ogk1_generation") == before.get("ogk1_generation"), "Gateway'in imzalı izni yerinde")
    if after.get("fw"):
        res.append((True, f"Yazılım: {after['fw']}" + ("" if after.get("fw") != before.get("fw") else " (öncekiyle aynı)")))
    return res


BATTERY = re.compile(r"BATTERY state=(\w+) percent_est=(\d+) mv=(\d+) raw=(\d+)")
BATTERY_SOURCE = re.compile(r"BATTERY SOURCE usb=(\w+) power=(\w+) quiet=(\w+) loaded=(\d+) "
                            r"calibrations=(\d+) scale_ppm=(\d+) learned=(\w+) charge_ends=(\d+)")
POWER_WORD = {"BATTERY": "pilde", "CHARGING": "şarj oluyor", "FULL": "dolu",
              "NO_BATTERY": "pil takılı değil", "UNKNOWN": "henüz belli değil"}


def parse_battery(lines: list[str]) -> dict:
    """BATTERY? cevabı (Pil 2): voltaj, USB, pilin durumu ve kartın kendi ölçeği."""
    out: dict = {}
    for line in lines:
        if m := BATTERY.search(line):
            out.update(state=m[1], percent=int(m[2]), mv=int(m[3]), raw=int(m[4]))
        elif m := BATTERY_SOURCE.search(line):
            out.update(usb=m[1] == "yes", power=m[2], quiet=m[3] == "yes", loaded=int(m[4]),
                       calibrations=int(m[5]), scale_ppm=int(m[6]), learned=m[7] == "yes",
                       charge_ends=int(m[8]))
        elif m := re.search(r"BATTERY REGULATOR dcdc=(\d+) at_boot=(\d+)", line):
            out.update(dcdc=int(m[1]), dcdc_at_boot=int(m[2]))
    return out


def battery_report(b: dict) -> list[str]:
    """Türkçe özet."""
    if "mv" not in b:
        return ["Cihaz pil ölçümünü bildirmedi (yazılımı eski olabilir)."]
    out = [f"USB: {'takılı' if b.get('usb') else 'yok'}",
           f"Pil: {POWER_WORD.get(b.get('power', 'UNKNOWN'), b.get('power'))} · {b['mv']} mV"
           + ("" if b.get("power") == "NO_BATTERY" else f" (%{100 if b.get('power') == 'FULL' else b['percent']})")]
    if "scale_ppm" in b:
        scale = (b["scale_ppm"] - 1_000_000) / 10_000
        out.append(f"Kartın ölçeği: {scale:+.1f}% " + ("(şarj sonundan öğrenildi)" if b.get("learned")
                                                      else "(henüz öğrenilmedi; ilk şarj bitince öğrenir)")
                   + f" · şarj sonu: {b.get('charge_ends', 0)}")
        out.append(f"Son okuma sakin anda mı: {'evet' if b.get('quiet') else 'hayır'} · "
                   f"yüklüyken alınan: {b.get('loaded', 0)} · kalibrasyon: {b.get('calibrations', 0)}")
    if "dcdc" in b:
        word = {3: "açık", 0: "kapalı"}.get(b["dcdc"], f"kısmen ({b['dcdc']})")
        was = {3: "açıktı", 0: "kapalıydı"}.get(b["dcdc_at_boot"], f"kısmendi ({b['dcdc_at_boot']})")
        out.append(f"Verimli regülatör (DC/DC): {word} · bu yazılımdan önce açılışta {was}")
    return out


def read_battery(port: str, seconds: float = 3.0) -> list[str]:
    import serial
    lines: list[str] = []
    with serial.Serial(port, 115200, timeout=0.25, write_timeout=2, exclusive=True) as ser:
        try:
            ser.rts = False
            ser.dtr = True
        except OSError:
            pass
        time.sleep(0.8)
        ser.reset_input_buffer()
        ser.write(b"BATTERY?\n")
        ser.flush()
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            text = ser.readline().decode("utf8", "replace").strip()
            if text:
                lines.append(text)
    return lines


FENCE_PAGE = re.compile(r"APP FENCE DIAG page=([AB]) classified=\w+ evidence=(\w+) all_ff=(\w+) tail_ff=(\w+)")
FENCE_COMMITTED = ("COMMITTED_CLEAR", "COMMITTED_CONFIGURED")


def parse_fence(lines: list[str]) -> dict:
    """APP FENCE DIAG? cevabı: iki sayfanın durumu ve bakım kilidi (yalnız okuma)."""
    out: dict = {}
    for line in lines:
        if m := FENCE_PAGE.search(line):
            out[m[1]] = {"evidence": m[2], "all_ff": m[3] == "yes", "tail_ff": m[4] == "yes"}
        elif line.startswith("APP FENCE DIAG maintenance="):
            out["maintenance"] = line.endswith("=yes")
    return out


def fence_verdict(d: dict) -> tuple[str, str]:
    """(durum, açıklama). durum: hazir | bos | kalinti | bakim | bilinmiyor.

    kalinti = hiçbir sayfada kayıtlı alan yok ama bölge boş değil: cihazdaki
    önceki (daha büyük) yazılımın artığı. Yalnız bu durumda hazırlanabilir.
    """
    a, b = d.get("A"), d.get("B")
    if not a or not b or "maintenance" not in d:
        return "bilinmiyor", "Cihaz alan belleği sorusuna cevap vermedi (yazılım eski olabilir)."
    committed = [p for p in (a, b) if p["evidence"] in FENCE_COMMITTED]
    if not d["maintenance"]:
        if committed:
            return "hazir", "Alan belleği hazır."
        return "bos", "Alan belleği boş; cihaz yeniden başlayınca kendiliğinden kurulur."
    if committed or "UNSUPPORTED_NEWER" in (a["evidence"], b["evidence"]):
        return "bakim", ("Alan belleği kilitli ama içinde kayıtlı alan (ya da daha yeni yazılımın "
                         "kaydı) var. Silinmez; incelenmesi gerekir.")
    return "kalinti", ("Alan belleği hiç kurulmamış: bölgede önceki yazılımın artığı var, "
                       "kayıtlı alan yok. Bu tasmaya alan yazılamaz; hazırlanması gerekir.")


def fence_exchange(port: str, command: str, until: str, seconds: float) -> list[str]:
    import serial
    lines: list[str] = []
    with serial.Serial(port, 115200, timeout=0.25, write_timeout=2, exclusive=True) as ser:
        try:
            ser.rts = False
            ser.dtr = True
        except OSError:
            pass
        time.sleep(0.8)
        ser.reset_input_buffer()
        ser.write((command + "\n").encode("ascii"))
        ser.flush()
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                text = ser.readline().decode("utf8", "replace").strip()
            except Exception:   # cihaz yeniden başladı, port kapandı
                break
            if text:
                lines.append(text)
            if text.startswith(until):
                break
    return lines


def role_command(role: str) -> str:
    """Boş cihazın rolü: kayıtlı hizmet niyeti (tasma: konum takibi; gateway: alıcı)."""
    return {"tasma": "APP SERVICES T", "gateway": "APP SERVICES A"}[role]


def send_role(port: str, role: str, seconds: float = 8.0) -> list[str]:
    import serial
    lines: list[str] = []
    with serial.Serial(port, 115200, timeout=0.25, write_timeout=2, exclusive=True) as ser:
        try:
            ser.rts = False
            ser.dtr = True
        except OSError:
            pass
        time.sleep(0.8)
        ser.reset_input_buffer()
        ser.write((role_command(role) + "\n").encode("ascii"))
        ser.flush()
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                text = ser.readline().decode("utf8", "replace").strip()
            except Exception:   # cihaz yeniden başladı, port kapandı
                break
            if text:
                lines.append(text)
            if text.startswith("ROLE restart"):
                break
    return lines


def read(port: str, seconds: float = 6.0) -> list[str]:
    import serial  # pyserial; PC'de kurulu (eski araçlar da kullanıyor)
    lines: list[str] = []
    with serial.Serial(port, 115200, timeout=0.25, write_timeout=2, exclusive=True) as ser:
        try:
            ser.rts = False
            ser.dtr = True   # USB CDC: cihaz konuşmaya başlasın
        except OSError:
            pass
        time.sleep(0.8)
        ser.reset_input_buffer()
        for cmd in ("BENCH?", "PRODUCT STORE?", "APP DEVICE?"):
            ser.write((cmd + "\n").encode("ascii"))
            ser.flush()
            end = time.monotonic() + seconds / 3
            while time.monotonic() < end:
                text = ser.readline().decode("utf8", "replace").strip()
                if text:
                    lines.append(text)
    return lines


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("oku")
    r.add_argument("--port", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--deneme", type=int, default=1, help="cihaz açılana kadar kaç kez denensin")
    c = sub.add_parser("karsilastir")
    c.add_argument("once")
    c.add_argument("sonra")
    r2 = sub.add_parser("rol")
    r2.add_argument("--port", required=True)
    r2.add_argument("rol", choices=("tasma", "gateway"))
    r3 = sub.add_parser("pil")
    r3.add_argument("--port", required=True)
    r3.add_argument("--izle", type=int, default=0, help="dakika boyunca her dakika bir satır yaz")
    r4 = sub.add_parser("alan")
    r4.add_argument("--port", required=True)
    r4.add_argument("--hazirla", action="store_true",
                    help="yalnız kayıtlı alan yokken: alan belleğinin iki sayfasını sil")
    a = ap.parse_args()
    if a.cmd == "alan":
        lines = fence_exchange(a.port, "APP FENCE DIAG?", "APP FENCE DIAG maintenance=", 8.0)
        for line in lines:
            if line.startswith("APP FENCE DIAG"):
                print("  " + line)
        state, text = fence_verdict(parse_fence(lines))
        print(text)
        if not a.hazirla:
            return 0 if state in ("hazir", "bos") else 1
        if state != "kalinti":
            print("Hazırlama yapılmadı: bu durumda silinecek bir artık yok ya da silmek güvenli değil.")
            return 0 if state in ("hazir", "bos") else 2
        print("YALNIZ alan belleğinin iki sayfası (0x0E5000..0x0E6FFF) silinecek.")
        print("Anahtar, kayıtlar, ayarlar ve gateway izni bu bölgede değildir; dokunulmaz.")
        print("Geri dönüş: silinen yalnız artık baytlardır; alanlar uygulamadan yeniden gönderilir.")
        if input("Onaylıyorsan HAZIRLA yaz: ").strip() != "HAZIRLA":
            print("İptal edildi.")
            return 0
        out = fence_exchange(a.port, "APP FENCE PREPARE CONFIRM", "APP FENCE PREPARE result=", 20.0)
        if not any(l.startswith("APP FENCE PREPARE result=PASS") for l in out):
            print("DUR: hazırlama tamamlanmadı:\n  " + "\n  ".join(out[-5:]), file=sys.stderr)
            return 2
        time.sleep(6)   # cihaz yeniden başlıyor, boş alan kaydını kuruyor
        for _ in range(10):
            try:
                after = fence_exchange(a.port, "APP FENCE DIAG?", "APP FENCE DIAG maintenance=", 8.0)
                state, text = fence_verdict(parse_fence(after))
                if state == "hazir":
                    print("Tamam: alan belleği kuruldu. Alanları uygulamadan gönderebilirsin.")
                    return 0
            except Exception:
                pass
            time.sleep(2)
        print("DUR: hazırlamadan sonra alan belleği hazır görünmedi: " + text, file=sys.stderr)
        return 2
    if a.cmd == "pil":
        b = parse_battery(read_battery(a.port))
        print("\n".join(battery_report(b)))
        for _ in range(a.izle):
            time.sleep(60)
            b = parse_battery(read_battery(a.port))
            if "mv" in b:
                print(time.strftime("%H:%M"), f"usb={'var' if b.get('usb') else 'yok'}",
                      f"durum={b.get('power')}", f"mv={b['mv']}", f"olcek_ppm={b.get('scale_ppm')}",
                      f"sarj_sonu={b.get('charge_ends', 0)}", flush=True)
        return 0 if "mv" in b else 2
    if a.cmd == "rol":
        info = parse(read(a.port))
        if info.get("kind") != "unassigned":
            print(f"DUR: bu cihaz kurulmamış değil ({KIND_WORD.get(info.get('kind'), info.get('kind'))}); "
                  "rolü değişmez.", file=sys.stderr)
            return 2
        gnss = info.get("gnss")
        if gnss == "yes" and a.rol == "gateway":
            print("Not: bu cihazda GPS var; çoğunlukla tasmadır.")
        if gnss == "no" and a.rol == "tasma":
            print("Not: bu cihazda GPS yok; tasma konum bulamaz.")
        lines = send_role(a.port, a.rol)
        if not any(l.startswith("ROLE chosen=") for l in lines):
            print("DUR: cihaz rolü kaydetmedi:\n  " + "\n  ".join(lines[-5:]), file=sys.stderr)
            return 2
        time.sleep(4)
        for _ in range(15):
            try:
                after = parse(read(a.port))
                if after.get("kind") in ("tracker", "gateway"):
                    print(f"Tamam: cihaz artık {KIND_WORD[after['kind']].lower()}.")
                    return 0
            except Exception:
                pass
            time.sleep(2)
        print("DUR: cihaz yeniden başladıktan sonra okunamadı.", file=sys.stderr)
        return 2
    if a.cmd == "oku":
        last, best = "", None
        for i in range(a.deneme):
            try:
                info = parse(read(a.port))
                if info.get("kind"):
                    best = info
                    if complete(info):
                        break
                    last = "kayıt hafızası meşgul"
                else:
                    last = "cihaz kendini tanıtmadı"
            except Exception as exc:  # port henüz yok / meşgul
                last = str(exc)
            if i + 1 < a.deneme:
                time.sleep(2)
        if best is None:
            print(f"DUR: cihaz okunamadı ({last})", file=sys.stderr)
            return 2
        # Eksik okuma da yazılır; karşılaştırma eksik kalanı SORUN olarak gösterir.
        with open(a.out, "w") as f:
            json.dump(best, f)
        print(f"Cihaz: {KIND_WORD.get(best['kind'], best['kind'])} {best.get('id')}"
              + (f" · yazılım {best['fw']}" if best.get("fw") else ""))
        return 0
    with open(a.once) as f:
        before = json.load(f)
    with open(a.sonra) as f:
        after = json.load(f)
    bad = 0
    for ok, text in compare(before, after):
        print(("  TAMAM  " if ok else "  SORUN  ") + text)
        bad += 0 if ok else 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
