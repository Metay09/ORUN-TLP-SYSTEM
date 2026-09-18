"""M7P4: pure-transform and apply()/restore() lifecycle tests for
patch_internalfs.py.

Follows the exact pattern already established by tests/r4/test_patch_wire.py:
a self-contained FIXTURE stands in for the real upstream file, and
`ORIGINAL_GIT_BLOB_SHA` is overridden to match that fixture's own hash, so
this test needs no installed framework package and cannot fail a fresh
checkout or CI runner that has not run `pio run` yet. The real installed
package (if present in this environment) is used only for one additional,
non-fatal bonus check that the *actual* pinned hash still matches -- its
absence is reported, not treated as a test failure.

Never modifies the developer's actual installed framework package: the
script is loaded as a plain module via importlib, whose fresh globals() do
not contain "Import", so patch_internalfs.py's own
`if "Import" in globals()` guard stays false and apply() is never invoked
as a side effect of importing it. apply()/restore() lifecycle tests (G-I)
run against a disposable temporary directory standing in for the framework
package -- never the developer's real ~/.platformio copy.

Run directly: python3 firmware/tests/m7/test_m7p4_patch_internalfs.py
Exits non-zero (uncaught exception) on any failure, matching this repo's
plain assert-based host test convention; prints one PASS line on success.
"""
import importlib.util
import json
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "patch_internalfs.py"
REAL_INTERNALFS = (
    Path.home()
    / ".platformio/packages/framework-arduinoadafruitnrf52"
    / "libraries/InternalFileSytem/src/InternalFileSystem.cpp"
)

# Reduced stand-in for the real upstream file: only the fragments
# transform()/relocation_ok() actually inspect, plus enough surrounding
# structure (the #ifdef/#else/#endif shape, a trailing use of each macro)
# to exercise replace_once()'s exact-match/exact-count contract realistically.
FIXTURE = '''#include "InternalFileSystem.h"
#include "flash/flash_nrf5x.h"

#ifdef NRF52840_XXAA
  #define LFS_FLASH_ADDR        0xED000
#else
  #define LFS_FLASH_ADDR        0x6D000
#endif

#define LFS_FLASH_TOTAL_SIZE  (7*FLASH_NRF52_PAGE_SIZE)
#define LFS_BLOCK_SIZE        128

static inline uint32_t lba2addr(uint32_t block)
{
  return ((uint32_t) LFS_FLASH_ADDR) + block * LFS_BLOCK_SIZE;
}

static struct lfs_config _InternalFSConfig =
{
  .block_count = LFS_FLASH_TOTAL_SIZE / LFS_BLOCK_SIZE,
};

InternalFileSystem InternalFS;
'''


def load_raw_module():
    spec = importlib.util.spec_from_file_location("patch_internalfs", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_module():
    module = load_raw_module()
    # Match tests/r4/test_patch_wire.py's established pattern: pin the
    # module's expected hash to this self-contained fixture instead of the
    # real (much larger) upstream file, so this test needs no installed
    # framework package.
    module.ORIGINAL_GIT_BLOB_SHA = module.git_blob_sha(FIXTURE)
    return module


def expect_raises(fn, *args):
    try:
        fn(*args)
    except RuntimeError:
        return
    raise AssertionError(f"expected RuntimeError from {fn.__name__}{args!r}")


def fake_core(tmp_root, internalfs_text):
    core = Path(tmp_root)
    pkg = core / "libraries" / "InternalFileSytem" / "src"
    pkg.mkdir(parents=True, exist_ok=True)
    (core / "package.json").write_text(json.dumps({"version": "1.10700.0"}))
    (pkg / "InternalFileSystem.cpp").write_text(internalfs_text)
    return core


def target_path(core):
    return core / "libraries/InternalFileSytem/src/InternalFileSystem.cpp"


def backup_path(core):
    return core / "libraries/InternalFileSytem/src/InternalFileSystem.cpp.orun-original"


def main():
    mod = load_module()

    # A: exact upstream source transforms correctly.
    patched = mod.transform(FIXTURE)
    assert patched.startswith(mod.MARKER)

    # B: result contains the relocated definitions (base 0x0EB000, 2 pages /
    # 0x2000 total).
    assert "#define LFS_FLASH_ADDR        0x0EB000" in patched
    assert "#define LFS_FLASH_TOTAL_SIZE  (2*FLASH_NRF52_PAGE_SIZE)" in patched
    assert mod.relocation_ok(patched)

    # C: existing history range is untouched by this transform -- the stock
    # 0xED000 definition is gone (relocated), the unrelated #else branch's
    # 0x6D000 (a different MCU family, never ORUN's target) is preserved
    # exactly as upstream wrote it, and the stock size definition is also
    # gone (not left dangling alongside the new one).
    assert "#define LFS_FLASH_ADDR        0xED000" not in patched
    assert "#define LFS_FLASH_TOTAL_SIZE  (7*FLASH_NRF52_PAGE_SIZE)" not in patched
    assert "  #define LFS_FLASH_ADDR        0x6D000\n" in patched

    # D: wrong upstream blob/hash fails closed.
    expect_raises(mod.transform, "completely unrelated source text")
    expect_raises(mod.transform, FIXTURE.replace("InternalFileSystem", "Tampered"))

    # E: missing target definition fails closed (replace_once with zero
    # matches -- the exact mechanism transform() relies on for both targets).
    expect_raises(mod.replace_once, "no such fragment anywhere", "#define LFS_FLASH_ADDR        0xED000\n", "x")

    # F: duplicate target definition fails closed (replace_once with more
    # than one match).
    expect_raises(mod.replace_once, "X\nX\n", "X\n", "Y\n")

    # G: already-patched + valid backup is idempotent (apply() twice ->
    # same resulting file, no error, restore() still gets back to original).
    with tempfile.TemporaryDirectory() as tmp:
        core = fake_core(tmp, FIXTURE)
        restore_a = mod.apply(core)
        once_patched = target_path(core).read_text()
        assert once_patched == patched
        restore_b = mod.apply(core)  # idempotent re-apply, same process
        assert target_path(core).read_text() == once_patched
        restore_b()
        assert target_path(core).read_text() == FIXTURE
        # restore_a would be a no-op now (marker no longer present); prove
        # it does not corrupt the already-restored file.
        restore_a()
        assert target_path(core).read_text() == FIXTURE

    # H: already-patched + missing/incorrect backup fails.
    with tempfile.TemporaryDirectory() as tmp:
        core = fake_core(tmp, patched)  # patched on disk, but no backup written
        expect_raises(mod.apply, core)
    with tempfile.TemporaryDirectory() as tmp:
        core = fake_core(tmp, patched)
        backup_path(core).write_text("not the real original")
        expect_raises(mod.apply, core)

    # I: restore returns the vendor file byte-for-byte to the original.
    with tempfile.TemporaryDirectory() as tmp:
        core = fake_core(tmp, FIXTURE)
        restore = mod.apply(core)
        assert target_path(core).read_text() != FIXTURE
        restore()
        assert target_path(core).read_text() == FIXTURE
        assert mod.git_blob_sha(target_path(core).read_text()) == mod.ORIGINAL_GIT_BLOB_SHA

    # Negative layout check (task section 10, pure-check form): the guard's
    # relocation_ok() must reject a default/overlapping-with-history
    # relocation just as it rejects genuinely stock source, and must accept
    # only the exact audited relocation.
    stock_relabeled_as_patched = mod.MARKER + FIXTURE  # marker without real relocation
    assert not mod.relocation_ok(stock_relabeled_as_patched)
    assert not mod.relocation_ok(FIXTURE)  # stock, no marker at all
    wrong_addr = patched.replace("0x0EB000", "0xED000")  # would overlap history
    assert not mod.relocation_ok(wrong_addr)
    stale_size_alongside_new = patched.replace(
        "InternalFileSystem.h\"\n#include \"flash/flash_nrf5x.h\"\n",
        "InternalFileSystem.h\"\n#include \"flash/flash_nrf5x.h\"\n"
        "#define LFS_FLASH_TOTAL_SIZE  (7*FLASH_NRF52_PAGE_SIZE)\n",
    )  # stale stock size line reintroduced alongside the new one
    assert not mod.relocation_ok(stale_size_alongside_new)
    assert mod.relocation_ok(patched)  # the one genuinely correct case

    # Bonus, non-fatal: if the real installed framework package happens to
    # be present in this environment, confirm its actual pinned hash (the
    # real ORIGINAL_GIT_BLOB_SHA constant the script itself declares, with
    # no fixture override) still matches -- reported, not required.
    fresh = load_raw_module()
    if REAL_INTERNALFS.exists():
        real_source = REAL_INTERNALFS.read_text()
        matches = fresh.git_blob_sha(real_source) == fresh.ORIGINAL_GIT_BLOB_SHA
        print(f"M7P4 bonus check: real installed framework blob hash matches pin: {matches}")
        assert matches, (
            "installed framework's InternalFileSystem.cpp no longer matches "
            "the pinned upstream blob; dependency drift -- re-audit before trusting the patch"
        )
    else:
        print("M7P4 bonus check: no installed framework package found; skipped (not required)")

    print("M7P4 patch_internalfs pure transform and lifecycle checks: PASS")


if __name__ == "__main__":
    main()
