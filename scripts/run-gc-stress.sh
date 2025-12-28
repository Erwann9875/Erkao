#!/usr/bin/env bash

set -e

EXE=""
SCRIPT="tests/stress/gc_deep.ek"

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
    --script)
      if [ -z "${2:-}" ]; then
        echo "Missing value for --script." >&2
        exit 64
      fi
      SCRIPT="$2"
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

if [ ! -f "$SCRIPT" ]; then
  echo "Stress test not found: $SCRIPT" >&2
  exit 1
fi

export ERKAO_GC_LOG=1
"$EXE" run "$SCRIPT"
