#!/usr/bin/env bash

set -e

resolve_erkao_exe() {
  local exe="${1:-}"
  if [ -n "$exe" ]; then
    echo "$exe"
    return
  fi

  local script_dir
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  local repo_root
  repo_root="$(cd "$script_dir/.." && pwd)"
  local build_root="$repo_root/build"
  local pattern="erkao"
  local default="$build_root/erkao"

  case "${OSTYPE:-}" in
    msys*|cygwin*|win32*)
      pattern="erkao.exe"
      default="$build_root/Debug/erkao.exe"
      ;;
  esac

  if [ -f "$default" ]; then
    echo "$default"
    return
  fi

  if [ -d "$build_root" ]; then
    local latest=""
    local latest_time=0
    while IFS= read -r file; do
      local mtime=""
      if mtime=$(stat -f %m "$file" 2>/dev/null); then
        :
      else
        mtime=$(stat -c %Y "$file" 2>/dev/null || echo 0)
      fi
      if [ -z "$mtime" ]; then
        mtime=0
      fi
      if [ "$mtime" -ge "$latest_time" ]; then
        latest_time="$mtime"
        latest="$file"
      fi
    done < <(find "$build_root" -type f -name "$pattern" 2>/dev/null)
    if [ -n "$latest" ]; then
      echo "$latest"
      return
    fi
  fi

  echo "$default"
}
