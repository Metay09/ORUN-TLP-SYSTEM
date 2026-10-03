#!/usr/bin/env python3
import json
import sys
from collections import Counter
from pathlib import Path


def rules_by_id(run):
    rules = {}
    tool = run.get("tool", {})
    components = [tool.get("driver", {})] + list(tool.get("extensions", []))
    for component in components:
        for rule in component.get("rules", []):
            rid = rule.get("id")
            if rid:
                rules[rid] = rule
    return rules


def resolve_level(result, rule):
    explicit = result.get("level")
    if explicit:
        return explicit
    default = rule.get("defaultConfiguration", {}).get("level")
    if default:
        return default
    # SARIF result.level defaults to warning when neither the result nor the
    # reportingDescriptor default configuration supplies a level.
    return "warning"


def main(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))

    resolved_results = []
    for run in data.get("runs", []):
        by_id = rules_by_id(run)
        driver_rules = run.get("tool", {}).get("driver", {}).get("rules", [])
        for result in run.get("results", []):
            rule = by_id.get(result.get("ruleId"))
            if rule is None:
                index = result.get("ruleIndex")
                if isinstance(index, int) and 0 <= index < len(driver_rules):
                    rule = driver_rules[index]
                else:
                    rule = {}
            resolved_results.append((result, rule, resolve_level(result, rule)))

    levels = Counter(level for _, _, level in resolved_results)

    print("CodeQL SARIF summary:")
    print(f"  total findings: {len(resolved_results)}")
    for level in ("error", "warning", "note", "none"):
        print(f"  {level}: {levels[level]}")

    if not resolved_results:
        print("  findings: none")
        return 0

    print("CodeQL findings:")
    for i, (result, _rule, level) in enumerate(resolved_results, start=1):
        rid = result.get("ruleId", "<unknown-rule>")
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
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: summarize_sarif.py <results.sarif>")
    raise SystemExit(main(sys.argv[1]))
