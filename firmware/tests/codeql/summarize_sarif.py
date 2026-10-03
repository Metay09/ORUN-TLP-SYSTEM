#!/usr/bin/env python3
import json
import sys
from collections import Counter
from pathlib import Path


def resolve_tool_component(run, reference):
    tool = run.get("tool", {})
    driver = tool.get("driver", {})
    extensions = list(tool.get("extensions", []))

    if not reference:
        return driver

    index = reference.get("index")
    if isinstance(index, int):
        if 0 <= index < len(extensions):
            return extensions[index]
        return {}

    guid = reference.get("guid")
    name = reference.get("name")
    if guid is None and name is None:
        return driver

    for component in [driver] + extensions:
        if guid is not None and component.get("guid") == guid:
            return component
        if guid is None and name is not None and component.get("name") == name:
            return component
    return {}


def resolve_rule(run, result):
    rule_reference = result.get("rule") or {}
    component = resolve_tool_component(run, rule_reference.get("toolComponent"))
    rules = component.get("rules", [])

    index = rule_reference.get("index")
    if index is None:
        index = result.get("ruleIndex")
    if isinstance(index, int) and 0 <= index < len(rules):
        return rules[index]

    rule_id = rule_reference.get("id") or result.get("ruleId")
    if rule_id:
        for rule in rules:
            if rule.get("id") == rule_id:
                return rule

    return {}


def resolve_level(result, rule):
    # SARIF 2.1.0: absent kind defaults to fail. If kind is present and is not
    # fail, severity does not apply and level is none.
    if result.get("kind", "fail") != "fail":
        return "none"

    explicit = result.get("level")
    if explicit:
        return explicit

    default = rule.get("defaultConfiguration", {}).get("level")
    if default:
        return default

    # For fail results, SARIF defaults level to warning when neither the result
    # nor the reportingDescriptor default configuration supplies a level.
    return "warning"


def main(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))

    resolved_results = []
    for run in data.get("runs", []):
        for result in run.get("results", []):
            rule = resolve_rule(run, result)
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
        rid = result.get("ruleId")
        if not rid:
            rid = (result.get("rule") or {}).get("id", "<unknown-rule>")
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
