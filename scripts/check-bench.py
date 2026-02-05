#!/usr/bin/env python3

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

BENCH_LINE = re.compile(r"^\s*([A-Za-z0-9_-]+)\s+(-?\d+(?:\.\d+)?)\s*$")


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def resolve_path(path: str) -> Path:
    p = Path(path)
    if p.is_absolute():
        return p
    return repo_root() / p


def resolve_exe(exe: str) -> Path:
    if exe:
        return resolve_path(exe)

    build_root = repo_root() / "build"
    if os.name == "nt":
        default = build_root / "Debug" / "erkao.exe"
        pattern = "erkao.exe"
    else:
        default = build_root / "erkao"
        pattern = "erkao"

    if default.is_file():
        return default

    latest = None
    latest_time = -1.0
    if build_root.is_dir():
        for candidate in build_root.rglob(pattern):
            if not candidate.is_file():
                continue
            try:
                mtime = candidate.stat().st_mtime
            except OSError:
                continue
            if mtime >= latest_time:
                latest = candidate
                latest_time = mtime
    return latest if latest else default


def discover_bench_files(bench_dir: Path) -> list[Path]:
    files: list[Path] = []
    for file in sorted(bench_dir.glob("*.ek")):
        if file.stem.startswith("bench_"):
            continue
        files.append(file)
    return files


def parse_bench_line(output: str) -> tuple[str, float] | None:
    match = BENCH_LINE.match(output.strip())
    if not match:
        return None
    return match.group(1), float(match.group(2))


def run_benchmark(exe: Path, bench_file: Path) -> tuple[str, float]:
    process = subprocess.run(
        [str(exe), "run", str(bench_file)],
        capture_output=True,
        text=True,
        check=False,
    )
    if process.returncode != 0:
        sys.stderr.write(f"Benchmark failed: {bench_file}\n")
        if process.stdout:
            sys.stderr.write(process.stdout)
            if not process.stdout.endswith("\n"):
                sys.stderr.write("\n")
        if process.stderr:
            sys.stderr.write(process.stderr)
            if not process.stderr.endswith("\n"):
                sys.stderr.write("\n")
        raise RuntimeError(f"benchmark process exited with {process.returncode}")

    lines = process.stdout.splitlines() + process.stderr.splitlines()
    parsed: list[tuple[str, float]] = []
    for line in lines:
        result = parse_bench_line(line)
        if result:
            parsed.append(result)

    if not parsed:
        raise RuntimeError(f"Could not parse benchmark output from {bench_file}")
    return parsed[-1]


def run_all(
    exe: Path,
    bench_dir: Path,
    repeat: int,
) -> tuple[dict[str, list[float]], dict[str, float]]:
    files = discover_bench_files(bench_dir)
    if not files:
        raise RuntimeError(f"No benchmark files found in {bench_dir}")

    all_runs: dict[str, list[float]] = {}
    medians: dict[str, float] = {}

    for bench_file in files:
        run_values: list[float] = []
        bench_name = None
        for index in range(repeat):
            name, value = run_benchmark(exe, bench_file)
            if bench_name is None:
                bench_name = name
            elif bench_name != name:
                raise RuntimeError(
                    f"Inconsistent benchmark name in {bench_file}: "
                    f"'{bench_name}' then '{name}'"
                )
            run_values.append(value)
            print(
                f"{bench_file.name} run {index + 1}/{repeat}: "
                f"{name} {value:.3f} ms"
            )

        assert bench_name is not None
        if bench_name in all_runs:
            raise RuntimeError(f"Duplicate benchmark name '{bench_name}'")

        all_runs[bench_name] = run_values
        medians[bench_name] = float(statistics.median(run_values))

    return all_runs, medians


def baseline_to_map(data: dict) -> dict[str, float]:
    benchmarks = data.get("benchmarks")
    if not isinstance(benchmarks, dict):
        raise RuntimeError("Baseline file must contain a 'benchmarks' object.")

    out: dict[str, float] = {}
    for name, value in benchmarks.items():
        if isinstance(value, dict):
            if "median_ms" not in value:
                raise RuntimeError(f"Benchmark '{name}' missing 'median_ms'.")
            ms = float(value["median_ms"])
        else:
            ms = float(value)
        out[name] = ms
    return out


def write_baseline(
    path: Path,
    repeat: int,
    max_regression_pct: float,
    min_slack_ms: float,
    runs: dict[str, list[float]],
    medians: dict[str, float],
) -> None:
    payload = {
        "version": 1,
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "repeat": repeat,
        "max_regression_pct": max_regression_pct,
        "min_slack_ms": min_slack_ms,
        "benchmarks": {
            name: {
                "median_ms": round(medians[name], 3),
                "runs_ms": [round(v, 3) for v in runs[name]],
            }
            for name in sorted(medians.keys())
        },
    }

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as file:
        json.dump(payload, file, indent=2, sort_keys=True)
        file.write("\n")


def compare_to_baseline(
    baseline: dict[str, float],
    medians: dict[str, float],
    max_regression_pct: float,
    min_slack_ms: float,
) -> int:
    failures = 0

    print("\nComparison:")
    print("name        baseline   median   limit    delta")
    print("----------- ---------- -------- -------- -------")

    for name in sorted(baseline.keys()):
        if name not in medians:
            print(f"{name:<11} MISSING")
            failures += 1
            continue

        base = baseline[name]
        measured = medians[name]
        limit = max(base * (1.0 + max_regression_pct / 100.0), base + min_slack_ms)
        delta_ms = measured - base
        delta_pct = 0.0 if base <= 0 else (delta_ms / base) * 100.0

        status = "PASS" if measured <= limit else "FAIL"
        if status == "FAIL":
            failures += 1

        print(
            f"{name:<11} {base:>10.3f} {measured:>8.3f} "
            f"{limit:>8.3f} {delta_pct:>+6.2f}% {status}"
        )

    extras = sorted(set(medians.keys()) - set(baseline.keys()))
    for name in extras:
        print(f"{name:<11} NEW benchmark (not in baseline)")

    return failures


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run and validate Erkao benchmarks.")
    parser.add_argument("--exe", default="", help="Path to erkao executable.")
    parser.add_argument("--bench-dir", default="bench", help="Benchmark directory.")
    parser.add_argument(
        "--baseline",
        default="bench/baseline.json",
        help="Baseline file path.",
    )
    parser.add_argument("--repeat", type=int, default=5, help="Runs per benchmark.")
    parser.add_argument(
        "--max-regression-pct",
        type=float,
        default=8.0,
        help="Maximum allowed regression percentage.",
    )
    parser.add_argument(
        "--min-slack-ms",
        type=float,
        default=20.0,
        help="Absolute slack in ms to reduce noise on short benchmarks.",
    )
    parser.add_argument(
        "--update-baseline",
        action="store_true",
        help="Write current results into the baseline file.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.repeat < 1:
        sys.stderr.write("repeat must be >= 1\n")
        return 2

    exe = resolve_exe(args.exe)
    if not exe.is_file():
        sys.stderr.write(f"Executable not found: {exe}\n")
        return 1

    bench_dir = resolve_path(args.bench_dir)
    if not bench_dir.is_dir():
        sys.stderr.write(f"Bench directory not found: {bench_dir}\n")
        return 1

    baseline_path = resolve_path(args.baseline)

    runs, medians = run_all(exe, bench_dir, args.repeat)
    print("\nMedians:")
    for name in sorted(medians.keys()):
        print(f"{name}: {medians[name]:.3f} ms")

    if args.update_baseline:
        write_baseline(
            baseline_path,
            args.repeat,
            args.max_regression_pct,
            args.min_slack_ms,
            runs,
            medians,
        )
        print(f"\nUpdated baseline: {baseline_path}")
        return 0

    if not baseline_path.is_file():
        sys.stderr.write(
            f"Baseline file not found: {baseline_path}\n"
            "Run with --update-baseline to create it.\n"
        )
        return 1

    with baseline_path.open("r", encoding="utf-8") as file:
        baseline_data = json.load(file)

    baseline = baseline_to_map(baseline_data)
    failures = compare_to_baseline(
        baseline,
        medians,
        args.max_regression_pct,
        args.min_slack_ms,
    )
    if failures > 0:
        print(f"\nPerformance gate failed: {failures} regression(s).")
        return 1

    print("\nPerformance gate passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
