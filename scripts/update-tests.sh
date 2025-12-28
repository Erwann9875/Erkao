#!/usr/bin/env bash

set -e

EXE=""
TESTS_DIR="tests"

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
    --tests-dir)
      if [ -z "${2:-}" ]; then
        echo "Missing value for --tests-dir." >&2
        exit 64
      fi
      TESTS_DIR="$2"
      shift 2
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 64
      ;;
  esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
args=()
if [ -n "$EXE" ]; then
  args+=(--exe "$EXE")
fi
if [ -n "$TESTS_DIR" ]; then
  args+=(--tests-dir "$TESTS_DIR")
fi

"$script_dir/run-tests.sh" --update "${args[@]}"
