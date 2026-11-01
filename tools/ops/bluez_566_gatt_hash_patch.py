#!/usr/bin/env python3
"""Create a patched copy of Debian BlueZ 5.66 src/shared/gatt-db.c.

The INPUT FILE IS NEVER MODIFIED. No bluetoothd, DBus, systemd or USB interaction.
This is a candidate until compiled and qualified against a real Gateway.
"""
from __future__ import annotations

import argparse
import difflib
from pathlib import Path
import re
import sys


def replace(original: str, pattern: str, replacement: str, label: str,
            expected: int = 1) -> str:
    updated, count = re.subn(pattern, replacement, original, flags=re.MULTILINE)
    if count != expected:
        raise ValueError(f"{label}: expected {expected} matching section(s), got {count}; no output written")
    return updated


def patch(source: str) -> str:
    if "static bool db_hash_update(void *user_data)" not in source:
        raise ValueError("BlueZ gatt-db.c db_hash_update function not found")
    if "hash.iov = new0(struct iovec, db->next_handle);" not in source:
        raise ValueError("Expected Debian BlueZ 5.66 allocation not found; no output written")

    result = replace(source,
        r"(struct hash_data\s*\{\s*struct iovec\s*\*iov;\s*)uint16_t i;(\s*\};)",
        r"\g<1>size_t i;\n\tsize_t capacity;\n\tbool failed;\g<2>",
        "hash_data bounds")
    result = replace(result,
        r"(static void gen_hash_m\([^)]*\)\s*\{\s*struct hash_data \*hash = user_data;\s*uint8_t \*data;\s*size_t len;)",
        r"\g<1>\n\n\tif (hash->failed)\n\t\treturn;",
        "gen_hash_m early error guard")
    result = replace(result,
        r"(\bdata = malloc\(2 \+ 2 \+ attr->value_len\);\n)(\s*put_le16\(attr->handle, data\);)",
        r"\g<1>\t\tif (!data) {\n\t\t\thash->failed = true;\n\t\t\treturn;\n\t\t}\n\g<2>",
        "malloc failures", expected=2)
    growth = """\t/* Do not use next_handle as a capacity: it can be stale. */
\tif (hash->i == hash->capacity) {
\t\tsize_t capacity = hash->capacity ? hash->capacity * 2 : 16;
\t\tstruct iovec *iov;

\t\tif (capacity > UINT16_MAX)
\t\t\tcapacity = UINT16_MAX;
\t\tif (hash->i >= capacity) {
\t\t\tfree(data);
\t\t\thash->failed = true;
\t\t\treturn;
\t\t}
\t\tiov = realloc(hash->iov, capacity * sizeof(*iov));
\t\tif (!iov) {
\t\t\tfree(data);
\t\t\thash->failed = true;
\t\t\treturn;
\t\t}
\t\thash->iov = iov;
\t\thash->capacity = capacity;
\t}

"""
    result = replace(result,
        r"(?m)^([ \t]*)(hash->iov\[hash->i\]\.iov_base = data;)",
        growth + r"\g<1>\g<2>",
        "bounded append")
    # The cleanup loop must not wrap at 65535; hash.i is size_t now.
    result = replace(result,
        r"(static bool db_hash_update\(void \*user_data\)\s*\{\s*struct gatt_db \*db = user_data;\s*struct hash_data hash;\s*)uint16_t i;",
        r"\g<1>size_t i;",
        "hash cleanup index width")
    result = replace(result,
        r"hash\.iov = new0\(struct iovec, db->next_handle\);\s*hash\.i = 0;",
        "hash.iov = NULL;\n\thash.i = 0;\n\thash.capacity = 0;\n\thash.failed = false;",
        "hash initialization")
    result = replace(result,
        r"bt_crypto_gatt_hash\(db->crypto, hash\.iov, db->next_handle, db->hash\);",
        "if (!hash.failed)\n\t\tbt_crypto_gatt_hash(db->crypto, hash.iov, hash.i, db->hash);",
        "hash actual entry count")
    if result == source:
        raise ValueError("Patch did not change source")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    source = args.input.resolve()
    destination = args.output.resolve()
    if source == destination:
        parser.error("--input and --output must differ; original stays untouched")
    if destination.exists():
        parser.error("--output already exists; refusing to overwrite")
    if not source.is_file():
        parser.error("--input source file does not exist")
    if "bluez-5.66" not in source.parts:
        parser.error("--input must be inside an extracted bluez-5.66 source tree")
    original = source.read_text(encoding="utf-8")
    try:
        changed = patch(original)
    except ValueError as e:
        parser.error(str(e))
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(changed, encoding="utf-8")
    print("SAFE: original file untouched:", source)
    print("CANDIDATE PATCH:", destination)
    print("\n".join(difflib.unified_diff(original.splitlines(),changed.splitlines(),fromfile="original/gatt-db.c",tofile="patched/gatt-db.c",lineterm="")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
