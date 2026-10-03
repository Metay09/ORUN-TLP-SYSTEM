#!/usr/bin/env python3
import json
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[3]
script = root / "firmware/tests/codeql/summarize_sarif.py"

sarif = {
    "version": "2.1.0",
    "runs": [{
        "tool": {
            "driver": {
                "name": "fixture",
                "rules": [
                    {"id": "explicit-error", "defaultConfiguration": {"level": "error"}},
                    {"id": "default-note", "defaultConfiguration": {"level": "note"}},
                    {"id": "sarif-default-warning"},
                ],
            },
            "extensions": [{
                "name": "extension",
                "rules": [
                    {"id": "extension-warning", "defaultConfiguration": {"level": "warning"}},
                ],
            }],
        },
        "results": [
            {"ruleId": "explicit-error", "ruleIndex": 0, "message": {"text": "a"}},
            {"ruleId": "default-note", "message": {"text": "b"}},
            {
                "rule": {"id": "extension-warning", "index": 0, "toolComponent": {"index": 0}},
                "message": {"text": "c"},
            },
            {"ruleId": "sarif-default-warning", "ruleIndex": 2, "message": {"text": "d"}},
            {
                "ruleId": "explicit-error",
                "ruleIndex": 0,
                "level": "note",
                "message": {"text": "override"},
            },
            {
                "ruleId": "explicit-error",
                "ruleIndex": 0,
                "kind": "pass",
                "message": {"text": "severity-not-applicable"},
            },
        ],
    }],
}

with tempfile.TemporaryDirectory(prefix="orun-sarif-") as td:
    path = Path(td) / "fixture.sarif"
    path.write_text(json.dumps(sarif), encoding="utf-8")
    proc = subprocess.run(
        ["python3", str(script), str(path)],
        check=True,
        text=True,
        capture_output=True,
    )

out = proc.stdout
assert "total findings: 6" in out
assert "error: 1" in out
assert "warning: 2" in out
assert "note: 2" in out
assert "none: 1" in out
assert "[warning] extension-warning" in out
assert "[note] explicit-error" in out
assert "[none] explicit-error" in out
print("DEVQ1 CodeQL SARIF severity resolution checks: PASS")
