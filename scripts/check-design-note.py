#!/usr/bin/env python3
from __future__ import annotations

import argparse
import fnmatch
import subprocess
import sys
from pathlib import Path

DEFAULT_CRITICAL = [
    "src/frontend/singlepass_parse.c",
    "src/runtime/exec.c",
    "src/typecheck/singlepass_types.c",
    "src/runtime/imports.c",
    "src/stdlib/stdlib_http.c",
]

def run_git(args: list[str]) -> str:
    process = subprocess.run(
        ["git", *args],
        text=True,
        capture_output=True,
        check=False,
    )
    if process.returncode != 0:
        raise RuntimeError(process.stderr.strip() or "git command failed")
    return process.stdout

def parse_changed_files(base: str, head: str) -> list[str]:
    out = run_git(["diff", "--name-only", f"{base}...{head}"])
    files = [line.strip().replace("\\", "/") for line in out.splitlines() if line.strip()]
    return files

def parse_numstat(base: str, head: str) -> dict[str, int]:
    out = run_git(["diff", "--numstat", f"{base}...{head}"])
    stats: dict[str, int] = {}
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) != 3:
            continue
        added, deleted, path = parts
        if added == "-" or deleted == "-":
            continue
        try:
            delta = int(added) + int(deleted)
        except ValueError:
            continue
        stats[path.replace("\\", "/")] = delta
    return stats

def is_critical(path: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatch(path, pattern) for pattern in patterns)

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, help="Base git revision (usually PR base SHA).")
    parser.add_argument("--head", default="HEAD", help="Head git revision.")
    parser.add_argument(
        "--critical",
        nargs="*",
        default=DEFAULT_CRITICAL,
        help="Critical file globs that require design notes for non-trivial changes.",
    )
    parser.add_argument(
        "--notes-glob",
        default="docs/design-notes/*.md",
        help="Glob that identifies design notes.",
    )
    parser.add_argument(
        "--min-lines",
        type=int,
        default=40,
        help="Minimum changed lines across critical files before requiring a note.",
    )
    args = parser.parse_args()

    try:
        changed_files = parse_changed_files(args.base, args.head)
        changed_stats = parse_numstat(args.base, args.head)
    except RuntimeError as exc:
        print(f"Design-note check skipped: {exc}")
        return 0

    if not changed_files:
        print("Design-note check: no changed files.")
        return 0

    design_notes = [path for path in changed_files if fnmatch.fnmatch(path, args.notes_glob)]
    critical_files = [path for path in changed_files if is_critical(path, args.critical)]
    critical_delta = sum(changed_stats.get(path, 0) for path in critical_files)

    print(f"Changed files: {len(changed_files)}")
    print(f"Critical files changed: {len(critical_files)}")
    print(f"Critical changed lines: {critical_delta}")
    print(f"Design notes changed: {len(design_notes)}")

    if critical_files and critical_delta >= args.min_lines and not design_notes:
        print("\nMissing design note for non-trivial critical-module changes.")
        print("Critical files:")
        for path in critical_files:
            print(f"  - {path}")
        print(
            "\nAdd a note under docs/design-notes/ (for example "
            "docs/design-notes/YYYYMMDD-<topic>.md)."
        )
        return 1

    return 0

if __name__ == "__main__":
    sys.exit(main())
