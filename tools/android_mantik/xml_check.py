"""Android kaynak XML'leri iyi biçimli mi (Gradle'ın ilk takıldığı yer).

PC'de Android SDK olmadan da çalışır: Gradle "--" içeren bir yorum ya da
kapanmamış bir etiket yüzünden APK derlemesini durdurmadan önce burada yakalanır
(2026-11-14: splash_colors.xml yorumundaki "--" APK derlemesini durdurdu).
"""
from pathlib import Path
import sys
import xml.dom.minidom

root = Path(sys.argv[1])
files = sorted(root.rglob("*.xml"))
bad = 0
for f in files:
    try:
        xml.dom.minidom.parse(str(f))
    except Exception as e:  # noqa: BLE001 - report every parse failure
        bad += 1
        print(f"FAIL {f.relative_to(root)}: {e}")
print(f"XML {len(files) - bad}/{len(files)} PASS")
sys.exit(1 if bad else 0)
