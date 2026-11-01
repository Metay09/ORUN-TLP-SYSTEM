# ORUN BLE — Debian BlueZ 5.66 GATT hash heap overflow: saha kanıtı ve güvenli düzeltme planı

2026-10-28 · **Tespit: doğrulandı; hotfix: hazırlanıyor, henüz derlenmedi veya uygulanmadı.**

## Kontrollü deneyin sonucu

- Debian Bookworm `bluez 5.66-1+deb12u2`, `bluetoothd` PID `1076173`.
- Gateway `09A462BD4B275BA5`, BLE `C5:DD:01:85:C9:4B`.
- Bağımsız Bleak: `DISCOVERED True`, `CONNECTED True`, `FRAMES 0`, D-Bus `UnknownObject` after daemon SIGABRT; PC Edge/Backend USB servisleri çalışıyor.
- 2026-10-28 04:37:03 +03 `coredumpctl`: `SIGABRT`, glibc `corrupted size vs. prev_size`, GDB `db_hash_update()` at `src/shared/gatt-db.c:371`.
- GDB `hash.iov = 0x55ac531ee510`, `hash.i = 16`, `sizeof(struct iovec) = 16`.
- glibc chunk header `hash.iov - 8 = 0xf1` => mask flags => chunk size `0xf0 = 240` bytes; 16B header => nominal 224B payload / 16B iovec => **14 descriptors**, but `hash.i=16`. Dump at `hash.iov+0xe0` and `+0xf0` shows additional `(pointer, length)` pairs, overwriting next chunk's header. OOB write is shown directly. Last 16B at `+0x100` contains unrelated payload. (**glibc usable_size may differ by 8B; not enough for a complete 15th descriptor**.)
- `__GI___libc_free(mem=0x55ac531ee510)` is precisely `free(hash.iov)`, which detects the already corrupted adjacent allocator metadata.

## Upstream matching source

Debian source: https://sources.debian.org/src/bluez/5.66-1%2Bdeb12u2/src/shared/gatt-db.c/

`struct hash_data { struct iovec *iov; uint16_t i; }`.

`gen_hash_m` pushes `hash->iov[hash->i]` **without capacity check**, then `hash->i++`.

`db_hash_update` does:

```c
hash.iov = new0(struct iovec, db->next_handle);
hash.i = 0;
gatt_db_foreach_service(db, NULL, service_gen_hash_m, &hash);
bt_crypto_gatt_hash(db->crypto, hash.iov, db->next_handle, db->hash);
for (i = 0; i < hash.i; i++)
    free(hash.iov[i].iov_base);
free(hash.iov);
```

The hash count is also wrong: `bt_crypto_gatt_hash(..., db->next_handle, ...)` uses a handle hint instead of the actual collected `hash.i`. This can hash zero entries and omit actual entries. In the captured crash there were 16 collected descriptors but malloc sized around 14 descriptors. The available core proves the out-of-bounds write at this site. Why `db->next_handle` was too small is not yet determined.

Crypto caller: https://sources.debian.org/src/bluez/5.66-1%2Bdeb12u2/src/shared/crypto.c/ (the `iov_len` is passed to `writev`).

## Repair direction (candidate; do not apply to running host untested)

Build an isolated, *exact Debian 5.66 source version* with a bounded, dynamically growing `hash_data.iov` vector. In `gen_hash_m`, reserve capacity before writing `hash->iov[hash->i]`, check allocation failures, and stop cleanly if growth fails. `db_hash_update` must pass **`hash.i`** (not `db->next_handle`) to `bt_crypto_gatt_hash`. The array length and counter should use `size_t` with a maximum 65535 GATT entries. Release all already allocated `iov_base` buffers on failure.

Regression tests should cover: 14-slot capacity then 16 descriptors, no out-of-bounds write; correct hash input count; empty hash; malloc/realloc failures; BLE scanner/connect/notifications; backend record and ACK custody. Use ASan/UBSan and source-level build before live activation.

Do **not** modify installed `/usr/libexec/bluetooth/bluetoothd` in place, mix Debian 13 packages into Bookworm, clear `/var/lib/bluetooth`, drop the Gateway's custody state, or run two PC Edges against the same SQLite.

## Deployment/rollback gate

1. Save installed BlueZ package/version and stock unit; isolate and compile patched daemon in dedicated test path.
2. Verify binary and sanitizer tests offline. Perform **one controlled BLE test** only after user agrees to host BlueZ service switchover; preserve the original unit executable for immediate rollback.
3. On BLE tests, require HELLO, DUMP END, then a fresh new test frame with backend STORED and Gateway authenticated bench ACK; no inference from old 101 USB records.
4. Restore stock BlueZ immediately on abnormal connection/crash; preserve coredump and confirm PC Edge USB/backend remain active.
5. Production authorization remains separate from PUBLIC TEST KEY bench.
