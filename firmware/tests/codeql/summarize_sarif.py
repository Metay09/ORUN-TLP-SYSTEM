#!/usr/bin/env python3
import json
import sys
from collections import Counter
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit("usage: summarize_sarif.py <results.sarif>")

path = Path(sys.argv[1])
data = json.loads(path.read_text(encoding="utf-8"))

results = []
for run in data.get("runs", []):
    rules = {}
    driver = run.get("tool", {}).get("driver", {})
    for rule in driver.get("rules", []):
        rid = rule.get("id")
        if rid:
            rules[rid] = rule
    for result in run.get("results", []):
        result["_rule"] = rules.get(result.get("ruleId"), {})
        results.append(result)

levels = Counter((r.get("level") or "none") for r in results)

print("CodeQL SARIF summary:")
print(f"  total findings: {len(results)}")
for level in ("error", "warning", "note", "none"):
    print(f"  {level}: {levels[level]}")

if not results:
    print("  findings: none")
    raise SystemExit(0)

print("CodeQL findings:")
for i, result in enumerate(results, start=1):
    rid = result.get("ruleId", "<unknown-rule>")
    level = result.get("level") or "none"
    message = result.get("message", {}).get("text", "").replace("\n", " ").strip()
    locations = result.get("locations") or []
    where = "<no-location>"
    if locations:
        physical = locations[0].get("physicalLocation", {})
        uri = physical.get("artifactLocation", {}).get("uri", "<unknown-file>")
        region = physical.get("region", {})
        line = region.get("startLine")
        where = f"{uri}:{line}" if line is not None else uri
    print(f"  {i}. [{level}] {rid} @ {where}")
    if message:
        print(f"     {message}")

print("Review every finding before merge; zero findings is not assumed by the runner.")
