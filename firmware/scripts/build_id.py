# Y1: yazılım sürümüne derlendiği git sürümünü ekler ("0.5-7b7c540"), böylece
# uygulamadaki canlı kartta ve USB'de cihazda hangi yazılımın çalıştığı görünür.
# Git yoksa ya da çalışma ağacı temiz değilse bunu da söyler ("+" eki).
import subprocess

Import("env")  # noqa: F821  (PlatformIO/SCons)

root = env.subst("$PROJECT_DIR")  # noqa: F821
try:
    sha = subprocess.check_output(["git", "rev-parse", "--short=7", "HEAD"], cwd=root,
                                  stderr=subprocess.DEVNULL).decode().strip()
    dirty = subprocess.call(["git", "diff", "--quiet", "HEAD", "--", "."], cwd=root,
                            stderr=subprocess.DEVNULL) != 0
    build_id = sha + ("+" if dirty else "")
except Exception:
    build_id = "nogit"
env.Append(CPPDEFINES=[("ORUN_BUILD_ID", env.StringifyMacro(build_id))])  # noqa: F821
