#!/usr/bin/env python3

import argparse
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')

def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent

def normalize(path: str) -> str:
    return path.replace("\\", "/")

def resolve_file(path: str) -> Path:
    p = Path(path)
    if p.is_absolute():
        return p
    return repo_root() / p

def collect_project_files() -> set[str]:
    out: set[str] = set()
    for root in ("src", "include"):
        root_path = repo_root() / root
        if not root_path.exists():
            continue
        for file in root_path.rglob("*"):
            if not file.is_file():
                continue
            if file.suffix.lower() not in (".c", ".h"):
                continue
            rel = normalize(str(file.relative_to(repo_root())))
            out.add(rel)
    return out

def build_basename_index(files: set[str]) -> dict[str, list[str]]:
    by_name: dict[str, list[str]] = {}
    for file in files:
        name = Path(file).name
        by_name.setdefault(name, []).append(file)
    return by_name

def layer_for(path: str, mappings: list[dict[str, str]]) -> str | None:
    winner = None
    winner_len = -1
    for item in mappings:
        prefix = normalize(item["prefix"])
        if path.startswith(prefix) and len(prefix) > winner_len:
            winner = item["layer"]
            winner_len = len(prefix)
    return winner

def resolve_include(
    include_name: str,
    current_file: str,
    known_files: set[str],
    by_basename: dict[str, list[str]],
) -> str | None:
    include_norm = normalize(include_name)
    current_dir = normalize(str(Path(current_file).parent))

    direct_candidates = [
        include_norm,
        normalize(str(Path(current_dir) / include_norm)),
        normalize(str(Path("src") / include_norm)),
        normalize(str(Path("include") / include_norm)),
    ]
    for candidate in direct_candidates:
        if candidate in known_files:
            return candidate

    if "/" in include_norm:
        return None

    matches = by_basename.get(include_norm, [])
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        in_same_dir = [m for m in matches if normalize(str(Path(m).parent)) == current_dir]
        if len(in_same_dir) == 1:
            return in_same_dir[0]
    return None

def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as file:
        return json.load(file)

def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as file:
        json.dump(payload, file, indent=2, sort_keys=True)
        file.write("\n")

def check_contract(
    contract: dict,
    allowlist: dict | None,
) -> tuple[list[dict], list[dict], list[dict], list[dict], int]:
    mappings = contract.get("module_layers", [])
    allowed = contract.get("allowed_dependencies", {})

    known_files = collect_project_files()
    by_basename = build_basename_index(known_files)

    violations: list[dict] = []
    unresolved: list[dict] = []
    scanned = 0

    for file in sorted(known_files):
        if not file.startswith("src/"):
            continue
        scanned += 1
        from_layer = layer_for(file, mappings)
        if not from_layer:
            continue

        path = repo_root() / file
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as exc:
            raise RuntimeError(f"Could not read {file}: {exc}") from exc

        for line_no, line in enumerate(lines, start=1):
            match = INCLUDE_RE.match(line)
            if not match:
                continue

            include_name = match.group(1)
            target = resolve_include(include_name, file, known_files, by_basename)
            if not target:
                unresolved.append(
                    {
                        "from": file,
                        "line": line_no,
                        "include": include_name,
                    }
                )
                continue

            to_layer = layer_for(target, mappings)
            if not to_layer:
                unresolved.append(
                    {
                        "from": file,
                        "line": line_no,
                        "include": include_name,
                        "resolved_to": target,
                    }
                )
                continue

            allowed_targets = set(allowed.get(from_layer, []))
            if "*" in allowed_targets or to_layer in allowed_targets:
                continue

            violations.append(
                {
                    "from": file,
                    "line": line_no,
                    "include": include_name,
                    "to": target,
                    "from_layer": from_layer,
                    "to_layer": to_layer,
                }
            )

    key = lambda item: (item["from"], item["to"])
    unique: dict[tuple[str, str], dict] = {}
    for item in violations:
        unique[key(item)] = item
    violations = [unique[k] for k in sorted(unique.keys())]

    allowed_keys: set[tuple[str, str]] = set()
    if allowlist:
        for item in allowlist.get("violations", []):
            if "from" in item and "to" in item:
                allowed_keys.add((item["from"], item["to"]))

    current_keys = {(v["from"], v["to"]) for v in violations}
    new_violations = [v for v in violations if (v["from"], v["to"]) not in allowed_keys]
    stale_allow = sorted(allowed_keys - current_keys)

    return violations, new_violations, unresolved, [
        {"from": from_file, "to": to_file}
        for from_file, to_file in stale_allow
    ], scanned

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Check architectural layer dependencies.")
    parser.add_argument(
        "--contract",
        default="docs/architecture_contract.json",
        help="Path to architecture contract JSON.",
    )
    parser.add_argument(
        "--allowlist",
        default="docs/architecture_violations.json",
        help="Path to allowed current violations JSON.",
    )
    parser.add_argument(
        "--update-allowlist",
        action="store_true",
        help="Write the current violation set to the allowlist file.",
    )
    parser.add_argument(
        "--fail-on-unresolved",
        action="store_true",
        help="Fail if include resolution is ambiguous or unknown.",
    )
    parser.add_argument(
        "--fail-on-stale",
        action="store_true",
        help="Fail if allowlist contains violations that no longer exist.",
    )
    return parser.parse_args()

def main() -> int:
    args = parse_args()
    contract_path = resolve_file(args.contract)
    allowlist_path = resolve_file(args.allowlist)

    if not contract_path.is_file():
        sys.stderr.write(f"Contract file not found: {contract_path}\n")
        return 1

    contract = load_json(contract_path)
    allowlist = load_json(allowlist_path) if allowlist_path.is_file() else {"violations": []}

    violations, new_violations, unresolved, stale_allow, scanned = check_contract(
        contract,
        allowlist,
    )

    print(f"Architecture check scanned {scanned} source files.")
    print(f"Violations: {len(violations)}")
    print(f"New violations: {len(new_violations)}")
    print(f"Unresolved includes: {len(unresolved)}")
    print(f"Stale allowlist entries: {len(stale_allow)}")

    if args.update_allowlist:
        payload = {
            "version": 1,
            "generated_at": datetime.now(timezone.utc).isoformat(),
            "violations": [
                {
                    "from": v["from"],
                    "to": v["to"],
                    "from_layer": v["from_layer"],
                    "to_layer": v["to_layer"],
                }
                for v in violations
            ],
        }
        write_json(allowlist_path, payload)
        print(f"Updated allowlist: {allowlist_path}")
        return 0

    if new_violations:
        print("\nNew architecture violations:")
        for item in new_violations:
            print(
                f"- {item['from']} -> {item['to']} "
                f"({item['from_layer']} -> {item['to_layer']})"
            )

    if unresolved:
        print("\nUnresolved includes:")
        for item in unresolved[:50]:
            if "resolved_to" in item:
                print(
                    f"- {item['from']}:{item['line']} include \"{item['include']}\" "
                    f"(resolved to {item['resolved_to']}, unmapped layer)"
                )
            else:
                print(
                    f"- {item['from']}:{item['line']} include \"{item['include']}\" "
                    "(unresolved)"
                )
        if len(unresolved) > 50:
            print(f"- ... and {len(unresolved) - 50} more")

    if stale_allow:
        print("\nStale allowlist entries:")
        for item in stale_allow:
            print(f"- {item['from']} -> {item['to']}")

    if new_violations:
        return 1
    if args.fail_on_unresolved and unresolved:
        return 1
    if args.fail_on_stale and stale_allow:
        return 1
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
