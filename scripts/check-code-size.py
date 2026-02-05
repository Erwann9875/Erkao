#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path
import re

FUNC_START_RE = re.compile(
    r"^\s*(?:[A-Za-z_][A-Za-z0-9_\s\*\(\)]*?\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*\{"
)
CONTROL_NAMES = {"if", "for", "while", "switch", "else", "do"}

@dataclass(frozen=True)
class Finding:
    kind: str
    key: str
    path: str
    detail: str

def load_baseline(path: Path) -> set[str]:
    if not path.exists():
        return set()
    allowed: set[str] = set()
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        allowed.add(line)
    return allowed

def write_baseline(path: Path, keys: set[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Baseline for scripts/check-code-size.py",
        "# Auto-generated entries represent accepted existing debt.",
        "# Format:",
        "#   file:<path>",
        "#   func:<path>:<name>:<start_line>",
        "",
    ]
    lines.extend(sorted(keys))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

def sanitize_lines(text: str) -> list[str]:
    lines: list[str] = []
    in_block_comment = False
    in_string = False
    in_char = False
    escape = False

    current: list[str] = []
    i = 0
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if ch == "\n":
            lines.append("".join(current))
            current = []
            i += 1
            continue

        if in_block_comment:
            if ch == "*" and nxt == "/":
                current.append(" ")
                current.append(" ")
                in_block_comment = False
                i += 2
            else:
                current.append(" ")
                i += 1
            continue

        if in_string:
            current.append(" ")
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == '"':
                in_string = False
            i += 1
            continue

        if in_char:
            current.append(" ")
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == "'":
                in_char = False
            i += 1
            continue

        if ch == "/" and nxt == "/":
            while i < len(text) and text[i] != "\n":
                current.append(" ")
                i += 1
            continue

        if ch == "/" and nxt == "*":
            current.append(" ")
            current.append(" ")
            in_block_comment = True
            i += 2
            continue

        if ch == '"':
            current.append(" ")
            in_string = True
            i += 1
            continue

        if ch == "'":
            current.append(" ")
            in_char = True
            i += 1
            continue

        current.append(ch)
        i += 1

    lines.append("".join(current))
    return lines

def analyze_file(path: Path, repo_root: Path, max_file_lines: int, max_func_lines: int) -> list[Finding]:
    findings: list[Finding] = []
    source = path.read_text(encoding="utf-8", errors="replace")
    raw_lines = source.splitlines()
    file_lines = len(raw_lines)
    rel_path = path.relative_to(repo_root).as_posix()

    if file_lines > max_file_lines:
        key = f"file:{rel_path}"
        detail = f"{rel_path} has {file_lines} lines (limit {max_file_lines})"
        findings.append(Finding("file", key, rel_path, detail))

    lines = sanitize_lines(source)

    brace_depth = 0
    in_function = False
    function_name = ""
    function_start = 0

    for index, line in enumerate(lines, start=1):
        stripped = line.strip()

        if not in_function and brace_depth == 0 and stripped and not stripped.startswith("#"):
            match = FUNC_START_RE.match(line)
            if match:
                name = match.group(1)
                if name not in CONTROL_NAMES:
                    in_function = True
                    function_name = name
                    function_start = index

        open_count = line.count("{")
        close_count = line.count("}")
        brace_depth += open_count - close_count
        if brace_depth < 0:
            brace_depth = 0

        if in_function and brace_depth == 0:
            length = index - function_start + 1
            if length > max_func_lines:
                key = f"func:{rel_path}:{function_name}:{function_start}"
                detail = (
                    f"{rel_path}:{function_start} function '{function_name}' has {length} lines "
                    f"(limit {max_func_lines})"
                )
                findings.append(Finding("func", key, rel_path, detail))
            in_function = False
            function_name = ""
            function_start = 0

    return findings

def collect_sources(root: Path) -> list[Path]:
    out: list[Path] = []
    for path in sorted(root.rglob("*.c")):
        if path.is_file():
            out.append(path)
    return out

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--src", type=Path, default=Path("src"))
    parser.add_argument("--baseline", type=Path, default=Path("docs/code_size_baseline.txt"))
    parser.add_argument("--max-file-lines", type=int, default=1800)
    parser.add_argument("--max-function-lines", type=int, default=220)
    parser.add_argument("--update-baseline", action="store_true")
    return parser.parse_args()

def main() -> int:
    args = parse_args()
    repo_root = Path.cwd()
    src_root = (repo_root / args.src).resolve()
    if not src_root.exists():
        print(f"Source root not found: {src_root}", file=sys.stderr)
        return 1

    findings: list[Finding] = []
    for path in collect_sources(src_root):
        findings.extend(
            analyze_file(
                path=path,
                repo_root=repo_root,
                max_file_lines=args.max_file_lines,
                max_func_lines=args.max_function_lines,
            )
        )

    keys = {finding.key for finding in findings}

    baseline_path = (repo_root / args.baseline).resolve()
    if args.update_baseline:
        write_baseline(baseline_path, keys)
        print(f"Updated baseline: {baseline_path}")
        print(f"Findings captured: {len(keys)}")
        return 0

    baseline = load_baseline(baseline_path)
    new_findings = sorted([f for f in findings if f.key not in baseline], key=lambda f: f.key)

    print(f"Code-size findings: {len(findings)}")
    print(f"Baseline entries: {len(baseline)}")
    print(f"New findings: {len(new_findings)}")

    if new_findings:
        print("\nNew code-size violations:")
        for finding in new_findings:
            print(f"  - {finding.detail} [{finding.key}]")
        print(
            "\nRefactor the code to satisfy limits, or (for existing debt only) "
            "update docs/code_size_baseline.txt."
        )
        return 1

    return 0

if __name__ == "__main__":
    sys.exit(main())
