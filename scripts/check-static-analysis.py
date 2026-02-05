#!/usr/bin/env python3
"""Fail CI when static analysis reports issues not present in baseline."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


CPP_RE = re.compile(r"^(.*?):(\d+):([^:]+):([^:]+):(.*)$")
CLANG_RE = re.compile(
    r"^(.*?):(\d+):(\d+):\s*(warning|error):\s*(.*?)\s*\[([^\]]+)\]\s*$"
)


def normalize_path(raw: str, repo_root: Path) -> str:
    text = raw.strip().replace("\\", "/")
    path = Path(text)
    if path.is_absolute():
        try:
            return path.resolve().relative_to(repo_root.resolve()).as_posix()
        except ValueError:
            return path.as_posix()
    if text.startswith("./"):
        text = text[2:]
    return text


def parse_cppcheck(path: Path, repo_root: Path) -> set[str]:
    findings: set[str] = set()
    if not path.exists():
        return findings
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = CPP_RE.match(line.strip())
        if not match:
            continue
        file_path, line_no, severity, rule_id, _message = match.groups()
        norm = normalize_path(file_path, repo_root)
        findings.add(f"cppcheck:{norm}:{line_no}:{severity}:{rule_id}")
    return findings


def parse_clang_tidy(path: Path, repo_root: Path) -> set[str]:
    findings: set[str] = set()
    if not path.exists():
        return findings
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = CLANG_RE.match(line.strip())
        if not match:
            continue
        file_path, line_no, _col_no, severity, _message, check_name = match.groups()
        norm = normalize_path(file_path, repo_root)
        findings.add(f"clang-tidy:{norm}:{line_no}:{severity}:{check_name}")
    return findings


def load_baseline(path: Path) -> set[str]:
    if not path.exists():
        return set()
    baseline: set[str] = set()
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        baseline.add(line)
    return baseline


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--cppcheck", required=True, type=Path)
    parser.add_argument("--clang-tidy", required=True, type=Path, dest="clang_tidy")
    args = parser.parse_args()

    repo_root = Path.cwd()
    baseline = load_baseline(args.baseline)
    findings = set()
    findings.update(parse_cppcheck(args.cppcheck, repo_root))
    findings.update(parse_clang_tidy(args.clang_tidy, repo_root))

    new_findings = sorted(findings - baseline)

    print(f"Static analysis findings: {len(findings)}")
    print(f"Baseline findings: {len(baseline)}")
    print(f"New findings: {len(new_findings)}")

    if new_findings:
        print("\nNew static analysis issues detected:")
        for item in new_findings:
            print(f"  - {item}")
        print(
            "\nIf these are accepted existing issues, add exact fingerprints "
            "to docs/static_analysis_baseline.txt."
        )
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
