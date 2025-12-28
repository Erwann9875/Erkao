#!/usr/bin/env bash

set -e

EXE=""
BENCH_DIR="bench"
REPEAT=1

while [ $# -gt 0 ]; do
  case "$1" in
    --exe)
      if [ -z "${2:-}" ]; then
        echo "Missing value for --exe." >&2
        exit 64
      fi
      EXE="$2"
      shift 2
      ;;
    --bench-dir)
      if [ -z "${2:-}" ]; then
        echo "Missing value for --bench-dir." >&2
        exit 64
      fi
      BENCH_DIR="$2"
      shift 2
      ;;
    --repeat)
      if [ -z "${2:-}" ]; then
        echo "Missing value for --repeat." >&2
        exit 64
      fi
      REPEAT="$2"
      shift 2
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 64
      ;;
  esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
cd "$repo_root"
. "$script_dir/resolve-erkao.sh"

EXE="$(resolve_erkao_exe "$EXE")"

if [ ! -f "$EXE" ]; then
  echo "Executable not found: $EXE" >&2
  exit 1
fi

if [ ! -d "$BENCH_DIR" ]; then
  echo "Bench directory not found: $BENCH_DIR" >&2
  exit 1
fi

if [ "$REPEAT" -lt 1 ]; then
  echo "Repeat must be >= 1" >&2
  exit 1
fi

files=()
while IFS= read -r file; do
  files+=("$file")
done < <(find "$BENCH_DIR" -type f -name "*.ek" | sort)

if [ "${#files[@]}" -eq 0 ]; then
  echo "No benchmark files found in $BENCH_DIR"
  exit 0
fi

for file in "${files[@]}"; do
  run=1
  while [ "$run" -le "$REPEAT" ]; do
    if [ "$REPEAT" -gt 1 ]; then
      echo "== $(basename "$file") (run $run/$REPEAT) =="
    else
      echo "== $(basename "$file") =="
    fi
    "$EXE" run "$file"
    run=$((run + 1))
  done
done
