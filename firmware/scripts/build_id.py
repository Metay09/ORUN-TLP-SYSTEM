# Y1: yazılım sürümüne derlendiği git sürümünü ekler ("0.5-7b7c540"), böylece
# uygulamadaki canlı kartta ve USB'de cihazda hangi yazılımın çalıştığı görünür.
# Git yoksa ya da çalışma ağacı temiz değilse bunu da söyler ("+" eki).
import subprocess

Import("env")  # noqa: F821  (PlatformIO/SCons)

root = env.subst("$PROJECT_DIR")  # noqa: F821
try:
    # firmware/ klasörünü değiştiren son commit (2026-11-11): yalnız uygulama
    # ya da araç değişince yazılımın sürümü değişmez, her şey yeniden
    # derlenmez, aynı sürüm ikinci cihaza derlenmeden yüklenir.
    sha = subprocess.check_output(["git", "log", "-1", "--format=%h", "--abbrev=7", "--", "."],
                                  cwd=root, stderr=subprocess.DEVNULL).decode().strip()
    dirty = subprocess.call(["git", "diff", "--quiet", "HEAD", "--", "."], cwd=root,
                            stderr=subprocess.DEVNULL) != 0
    build_id = sha + ("+" if dirty else "")
except Exception:
    build_id = "nogit"
env.Append(CPPDEFINES=[("ORUN_BUILD_ID", env.StringifyMacro(build_id))])  # noqa: F821
