#!/usr/bin/env python3
import importlib.util
import json
import tempfile
from pathlib import Path

SCRIPT = Path(__file__).parents[2] / "scripts" / "patch_loop_stack.py"
spec = importlib.util.spec_from_file_location("patch_loop_stack", SCRIPT)
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)

FIXTURE = '''void initVariant() { }

#define LOOP_STACK_SZ       (256*4)
#define CALLBACK_STACK_SZ   (256*3)

  xTaskCreate(loop_task, "loop", LOOP_STACK_SZ, NULL, TASK_PRIO_LOW, &_loopHandle);
'''

# The pinned fingerprint is the real upstream file, not this fixture.
assert patch.ORIGINAL_GIT_BLOB_SHA == "7e3e95f82be564469ef6489e84419308a2727c60"

# An unrecognized source fails closed before anything is written.
try:
    patch.transform(FIXTURE)
    raise AssertionError("unrecognized upstream source must fail closed")
except RuntimeError:
    pass

patch.ORIGINAL_GIT_BLOB_SHA = patch.git_blob_sha(FIXTURE)
patched = patch.transform(FIXTURE)
assert patched.startswith(patch.MARKER)
assert "#define LOOP_STACK_SZ       (256*8)\n" in patched
assert "(256*4)" not in patched
# Only the loop task changes; the callback task and the rest are untouched.
assert "#define CALLBACK_STACK_SZ   (256*3)\n" in patched
assert patched[len(patch.MARKER):].replace("(256*8)", "(256*4)") == FIXTURE

try:
    patch.transform(FIXTURE + "// changed\n")
    raise AssertionError("changed upstream source must fail closed")
except RuntimeError:
    pass

# apply(): patch once, keep an exact backup, refuse a tampered patched file.
with tempfile.TemporaryDirectory() as tmp:
    core = Path(tmp)
    (core / "cores/nRF5").mkdir(parents=True)
    (core / "package.json").write_text(json.dumps({"version": "1.10700.0"}))
    target = core / "cores/nRF5/main.cpp"
    target.write_text(FIXTURE)
    patch.apply(core)
    assert target.read_text() == patched
    assert (core / "cores/nRF5/main.cpp.orun-original").read_text() == FIXTURE
    patch.apply(core)  # idempotent on an already patched tree
    assert target.read_text() == patched
    target.write_text(patched + "// tampered\n")
    try:
        patch.apply(core)
        raise AssertionError("tampered patched source must fail closed")
    except RuntimeError:
        pass

    (core / "package.json").write_text(json.dumps({"version": "1.10800.0"}))
    try:
        patch.apply(core)
        raise AssertionError("other core version must fail closed")
    except RuntimeError:
        pass

print("R4 pinned 8 KiB loop-task stack transform guards: PASS")
