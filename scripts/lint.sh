#!/usr/bin/env bash

set -e

EXE=""
DIRS=("tests" "examples")

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
    --dir)
      if [ -z "${2:-}" ]; then
        echo "Missing value for --dir." >&2
        exit 64
      fi
      DIRS+=("$2")
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

files=()
for dir in "${DIRS[@]}"; do
  if [ -d "$dir" ]; then
    while IFS= read -r file; do
      files+=("$file")
    done < <(find "$dir" -type f -name "*.ek")
  fi
done

if [ "${#files[@]}" -eq 0 ]; then
  echo "No .ek files found."
  exit 0
fi

args=("lint")
args+=("${files[@]}")

"$EXE" "${args[@]}"
