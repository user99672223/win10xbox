#!/usr/bin/env python3
"""Fail a CI job when ctest "passed" without running everything.

ctest counts a skipped test (SKIP_RETURN_CODE / SKIP_REGULAR_EXPRESSION) as
passed and prints "100% tests passed" while the guest end-to-end tests never
ran (docs/HANDOFF.md, trap 1). This compares what ctest knows about
(`ctest --show-only=json-v1`) with what it actually ran (the JUnit file written
by `ctest --output-junit`):

  * every registered test must have run and passed: a test that was skipped,
    not run, disabled or missing from the JUnit file fails the gate;
  * every label given with --require-label must have at least --min tests (or
    the number given as LABEL=N), so a label that silently lost its tests (or a
    configure that registered none) fails too. The counts come from ctest
    itself, never from a hard-coded list of test names.

  check_ctest_run.py --build <dir> --junit <file> [--require-label L[=N]]...

The JUnit path is relative to the build directory when it is not absolute,
exactly as ctest resolves --output-junit. Writes a table to
$GITHUB_STEP_SUMMARY when set. Exit status 0 = gate passed.
"""
import argparse
import json
import os
import subprocess
import sys
import xml.etree.ElementTree as ET


def registered_tests(build):
    out = subprocess.run(["ctest", "--test-dir", build, "--show-only=json-v1"],
                         check=True, capture_output=True, text=True).stdout
    tests = {}
    for t in json.loads(out).get("tests", []):
        labels = []
        for p in t.get("properties", []):
            if p.get("name") == "LABELS":
                labels = list(p.get("value") or [])
        tests[t["name"]] = labels
    return tests


def junit_results(path):
    res = {}
    for tc in ET.parse(path).getroot().iter("testcase"):
        status = tc.get("status", "")
        if tc.find("skipped") is not None:
            status = "skipped (" + (tc.find("skipped").get("message") or "") + ")"
        elif tc.find("failure") is not None:
            status = "failed"
        elif status == "run":
            status = "passed"
        res[tc.get("name")] = status
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--junit", required=True)
    ap.add_argument("--require-label", action="append", default=[],
                    help="LABEL or LABEL=N: at least N (default --min) registered tests carry LABEL, and all ran")
    ap.add_argument("--min", type=int, default=1)
    a = ap.parse_args()

    junit = a.junit if os.path.isabs(a.junit) else os.path.join(a.build, a.junit)
    tests = registered_tests(a.build)
    if not os.path.exists(junit):
        print(f"::error::{junit} missing: ctest did not run (or ran without --output-junit)")
        return 1
    results = junit_results(junit)

    problems = []
    if not tests:
        problems.append("ctest has no tests registered at all")
    for name in sorted(tests):
        st = results.get(name, "not run (absent from the JUnit file)")
        if st != "passed":
            problems.append(f"{name}: {st}")

    rows = []
    for spec in a.require_label:
        label, _, n = spec.partition("=")
        want = int(n) if n else a.min
        names = [t for t, labels in tests.items() if label in labels]
        ran = [t for t in names if results.get(t) == "passed"]
        rows.append((label, len(names), len(ran), want))
        if len(names) < want:
            problems.append(f"label {label}: {len(names)} test(s) registered, at least {want} required")

    total_passed = sum(1 for t in tests if results.get(t) == "passed")
    print(f"ctest gate: {len(tests)} registered, {total_passed} ran and passed")
    for label, reg, ran, want in rows:
        print(f"  label {label:<18} registered {reg:>3}   ran+passed {ran:>3}   required >= {want}")
    for p in problems:
        print(f"::error::ctest gate: {p}")

    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write(f"### ctest gate ({a.build})\n\n")
            f.write(f"{len(tests)} tests registered, {total_passed} ran and passed, "
                    f"{len(problems)} problem(s).\n\n")
            if rows:
                f.write("| label | registered | ran and passed | required |\n|---|---:|---:|---:|\n")
                for label, reg, ran, want in rows:
                    f.write(f"| {label} | {reg} | {ran} | >= {want} |\n")
                f.write("\n")
            for p in problems:
                f.write(f"* {p}\n")
            f.write("\n")
    if problems:
        print(f"ctest gate: FAIL ({len(problems)} problem(s)); a skipped test is a failed gate, see docs/TESTING.md")
        return 1
    print("ctest gate: PASS (nothing skipped, nothing missing)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
