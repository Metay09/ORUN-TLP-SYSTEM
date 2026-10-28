#!/usr/bin/env bash
# ORUN BlueZ 5.66 GATT hotfix — isolated build candidate only.
# No sudo, package install, service restart, or access to USB/ORUN databases.
set -euo pipefail
umask 077

WORK="$HOME/ORUN-BLUEZ-566-HOTFIX"
ORIGINAL="$WORK/bluez-5.66"
PATCHED="$WORK/gatt-db.patched-v2.c"

echo "=== ORUN BLUEZ 5.66 ISOLATED BUILD ==="
test -f "$ORIGINAL/src/shared/gatt-db.c" || {
  echo "ERROR: original BlueZ 5.66 source not found: $ORIGINAL" >&2; exit 1;
}
test -s "$PATCHED" || {
  echo "ERROR: v2 candidate source not found: $PATCHED" >&2; exit 1;
}
test -f "$ORIGINAL/configure.ac" && test -x "$ORIGINAL/bootstrap" || {
  echo "ERROR: BlueZ 5.66 configure.ac/bootstrap missing" >&2; exit 1;
}
command -v make >/dev/null
command -v cc >/dev/null
for tool in aclocal autoheader libtoolize automake autoconf; do
  command -v "$tool" >/dev/null || {
    echo "ERROR: missing GNU Autotools command: $tool" >&2
    echo "On Debian 12: sudo apt-get install --no-install-recommends autoconf automake libtool" >&2
    exit 1
  }
done
command -v pkg-config >/dev/null || {
  echo "ERROR: pkg-config missing. Install only required build dependencies." >&2; exit 1;
}
# BlueZ daemon profile objects still include the system Bluetooth API headers.
# Fail before bootstrap/configure rather than halfway through a lengthy compile.
if [[ ! -f /usr/include/bluetooth/bluetooth.h ]]; then
  echo "ERROR: missing /usr/include/bluetooth/bluetooth.h" >&2
  echo "On Debian 12: sudo apt-get install --no-install-recommends libbluetooth-dev" >&2
  exit 1
fi
pkg-config --exists glib-2.0 dbus-1 || {
  echo "ERROR: missing build headers: glib-2.0 and/or dbus-1." >&2
  echo "On Debian 12: sudo apt-get install --no-install-recommends pkg-config make libglib2.0-dev libdbus-1-dev" >&2
  exit 1
}
if [[ -e "$ORIGINAL/.libs" || -e "$ORIGINAL/Makefile" ]]; then
  echo "ERROR: upstream source tree was previously configured; abort instead of guessing" >&2
  exit 1
fi
if cmp -s "$ORIGINAL/src/shared/gatt-db.c" "$PATCHED"; then
  echo "ERROR: candidate is identical to original" >&2; exit 1;
fi
grep -Fq 'hash.capacity = 0;' "$PATCHED" || {
  echo "ERROR: patched source missing capacity initialization" >&2; exit 1;
}
grep -Fq 'bt_crypto_gatt_hash(db->crypto, hash.iov, hash.i, db->hash)' "$PATCHED" || {
  echo "ERROR: patched source missing correct hash count" >&2; exit 1;
}

BUILDDIR="$(mktemp -d "$WORK/build-candidate.XXXXXX")"
echo "BUILD PATH: $BUILDDIR"
cp -a "$ORIGINAL/." "$BUILDDIR/"
cp "$PATCHED" "$BUILDDIR/src/shared/gatt-db.c"

cd "$BUILDDIR"
echo "=== BOOTSTRAP: generate configure ==="
./bootstrap >"$WORK/bluez-bootstrap.log" 2>&1 || {
  echo "BOOTSTRAP: FAIL (last 80 lines)"
  tail -n 80 "$WORK/bluez-bootstrap.log"
  exit 1
}
test -x "./configure" || {
  echo "BOOTSTRAP: configure was not generated" >&2; exit 1;
}
echo "BOOTSTRAP: PASS"

echo "=== CONFIGURE ==="
./configure \
  --prefix=/usr \
  --sysconfdir=/etc \
  --localstatedir=/var \
  --disable-tools \
  --disable-client \
  --disable-monitor \
  --disable-cups \
  --disable-obex \
  --disable-manpages \
  --disable-udev \
  --disable-systemd \
  >"$WORK/bluez-configure.log" 2>&1 || {
  echo "CONFIGURE: FAIL (last 70 lines)"
  tail -n 70 "$WORK/bluez-configure.log"
  exit 1
}
echo "CONFIGURE: PASS"

echo "=== COMPILE bluetoothd (candidate only) ==="
make -j2 src/bluetoothd >"$WORK/bluez-build.log" 2>&1 || {
  echo "BUILD: FAIL (last 100 lines)"
  tail -n 100 "$WORK/bluez-build.log"
  exit 1
}
test -x "$BUILDDIR/src/bluetoothd"
echo "BUILD: PASS"
echo "CANDIDATE_BINARY: $BUILDDIR/src/bluetoothd"
sha256sum "$BUILDDIR/src/bluetoothd"
"$BUILDDIR/src/bluetoothd" --version || true

echo "=== INTEGRITY: host services untouched ==="
systemctl is-active bluetooth.service || true
systemctl --user is-active orun-pc-edge.service || true
systemctl --user is-active orun-backend.service || true
echo "=== CANDIDATE BUILT; NOT INSTALLED, NOT TESTED ON BLE ==="
