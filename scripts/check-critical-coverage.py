#!/usr/bin/env python3
"""Enforce minimum line coverage for critical modules."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


DEFAULT_CRITICAL = [
    "src/frontend/singlepass_parse.c",
    "src/runtime/exec.c",
    "src/typecheck/singlepass_types.c",
    "src/runtime/imports.c",
    "src/stdlib/stdlib_http.c",
]


def normalize(path: str) -> str:
    return path.replace("\\", "/")


def line_stats(file_entry: dict) -> tuple[int, int]:
    covered = file_entry.get("line_covered")
    total = file_entry.get("line_total")
    if isinstance(covered, int) and isinstance(total, int):
        return covered, total

    lines = file_entry.get("lines")
    if not isinstance(lines, list):
        return 0, 0
    line_total = 0
    line_covered = 0
    for line in lines:
        if not isinstance(line, dict):
            continue
        if line.get("gcovr/noncode"):
            continue
        line_total += 1
        if int(line.get("count", 0)) > 0:
            line_covered += 1
    return line_covered, line_total


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--coverage-json", required=True, type=Path)
    parser.add_argument("--critical", nargs="*", default=DEFAULT_CRITICAL)
    parser.add_argument("--min-line-coverage", type=float, default=5.0)
    args = parser.parse_args()

    if not args.coverage_json.exists():
        print(f"Coverage file not found: {args.coverage_json}", file=sys.stderr)
        return 1

    payload = json.loads(args.coverage_json.read_text(encoding="utf-8"))
    files = payload.get("files", [])
    by_path: dict[str, dict] = {}
    for item in files:
        if not isinstance(item, dict):
            continue
        path = item.get("file")
        if not isinstance(path, str):
            continue
        by_path[normalize(path)] = item

    failures = 0
    print("Critical coverage:")
    print("file                                covered/total   percent   status")
    print("----------------------------------- -------------- --------- -------")

    for file_path in args.critical:
        norm = normalize(file_path)
        entry = by_path.get(norm)
        if not entry:
            print(f"{norm:<35} {'missing':>14} {'0.00%':>9} FAIL")
            failures += 1
            continue

        covered, total = line_stats(entry)
        percent = 0.0 if total <= 0 else (100.0 * covered / total)
        status = "PASS" if percent >= args.min_line_coverage else "FAIL"
        if status == "FAIL":
            failures += 1
        print(f"{norm:<35} {covered:>6}/{total:<7} {percent:>8.2f}% {status}")

    if failures:
        print(
            f"\nCoverage gate failed: {failures} critical module(s) below "
            f"{args.min_line_coverage:.2f}% line coverage."
        )
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
