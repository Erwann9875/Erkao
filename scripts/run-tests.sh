#!/usr/bin/env bash

set -e

EXE=""
TESTS_DIR="tests"
UPDATE=0
WRITE_ACTUAL=0

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
    --update)
      UPDATE=1
      shift
      ;;
    --write-actual)
      WRITE_ACTUAL=1
      shift
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

if [ ! -d "$TESTS_DIR" ]; then
  echo "Tests directory not found: $TESTS_DIR" >&2
  exit 1
fi

export ERKAO_STACK_TRACE=1

http_test_enabled=1
if [ -n "${ERKAO_HTTP_TEST:-}" ]; then
  setting="$(printf "%s" "$ERKAO_HTTP_TEST" | tr '[:upper:]' '[:lower:]')"
  case "$setting" in
    0|false|no|off) http_test_enabled=0 ;;
  esac
fi

http_pid=""
http_stdout=""
http_stderr=""
http_port=""

cleanup() {
  if [ -n "$http_pid" ] && kill -0 "$http_pid" 2>/dev/null; then
    kill "$http_pid" 2>/dev/null || true
    wait "$http_pid" 2>/dev/null || true
  fi
  if [ -n "$http_stdout" ] && [ -f "$http_stdout" ]; then
    rm -f "$http_stdout"
  fi
  if [ -n "$http_stderr" ] && [ -f "$http_stderr" ]; then
    rm -f "$http_stderr"
  fi
}
trap cleanup EXIT

if [ "$http_test_enabled" -eq 1 ]; then
  server_path="$TESTS_DIR/http_server.ek"
  if [ ! -f "$server_path" ]; then
    echo "HTTP test server not found: $server_path" >&2
    exit 1
  fi

  http_stdout="$(mktemp)"
  http_stderr="$(mktemp)"
  "$EXE" run "$server_path" >"$http_stdout" 2>"$http_stderr" &
  http_pid=$!

  for _ in $(seq 1 60); do
    if ! kill -0 "$http_pid" 2>/dev/null; then
      break
    fi
    if [ -f "$http_stdout" ]; then
      line="$(grep -E "http\.serve listening on http://127\.0\.0\.1:[0-9]+" "$http_stdout" | head -n 1 || true)"
      if [ -n "$line" ]; then
        http_port="$(printf "%s" "$line" | sed -n 's/.*http:\/\/127\.0\.0\.1:\([0-9]*\).*/\1/p')"
        if [ -n "$http_port" ]; then
          break
        fi
      fi
    fi
    sleep 0.8
  done

  if [ -z "$http_port" ]; then
    details=""
    if [ -f "$http_stdout" ]; then
      stdout_text="$(cat "$http_stdout")"
      if [ -n "$stdout_text" ]; then
        details="${details}stdout:\n${stdout_text}\n"
      fi
    fi
    if [ -f "$http_stderr" ]; then
      stderr_text="$(cat "$http_stderr")"
      if [ -n "$stderr_text" ]; then
        details="${details}stderr:\n${stderr_text}\n"
      fi
    fi
    if [ -n "$details" ]; then
      echo -e "HTTP test server failed to start.\n$details" >&2
    else
      echo "HTTP test server failed to start." >&2
    fi
    exit 1
  fi

  check_port() {
    if command -v nc >/dev/null 2>&1; then
      nc -z 127.0.0.1 "$1" >/dev/null 2>&1
      return $?
    fi
    (echo > /dev/tcp/127.0.0.1/"$1") >/dev/null 2>&1
    return $?
  }

  ready=0
  for _ in $(seq 1 10); do
    if ! kill -0 "$http_pid" 2>/dev/null; then
      break
    fi
    if check_port "$http_port"; then
      ready=1
      break
    fi
    sleep 0.5
  done

  if [ "$ready" -ne 1 ]; then
    echo "HTTP test server started on port $http_port but refused connection." >&2
    exit 1
  fi

  export ERKAO_HTTP_TEST_PORT="$http_port"
fi

tests=()
while IFS= read -r file; do
  tests+=("$file")
done < <(find "$TESTS_DIR" -maxdepth 1 -type f -name "*.ek" ! -name "http_server.ek" | sort)

if [ "${#tests[@]}" -eq 0 ]; then
  echo "No tests found in $TESTS_DIR"
  exit 0
fi

if [ "$http_test_enabled" -eq 0 ]; then
  filtered=()
  for test in "${tests[@]}"; do
    if [ "$(basename "$test")" != "15_http.ek" ]; then
      filtered+=("$test")
    fi
  done
  tests=("${filtered[@]}")
fi

failed=0
updated=0

for test in "${tests[@]}"; do
  expected_path="${test%.ek}.out"
  if [ "$UPDATE" -ne 1 ] && [ ! -f "$expected_path" ]; then
    echo "Missing expected output: $expected_path"
    failed=$((failed + 1))
    continue
  fi

  test_rel="$test"
  if [[ "$test_rel" == "$repo_root/"* ]]; then
    test_rel="${test_rel#$repo_root/}"
  fi
  test_rel="${test_rel#./}"

  output="$("$EXE" run "$test_rel" 2>&1 || true)"
  output="$(printf "%s" "$output" | tr -d '\r')"
  output="$(printf "%s" "$output" | sed -e 's/[[:space:]]*$//')"

  if [ "$UPDATE" -eq 1 ]; then
    expected=""
    if [ -f "$expected_path" ]; then
      expected="$(cat "$expected_path")"
      expected="$(printf "%s" "$expected" | tr -d '\r')"
      expected="$(printf "%s" "$expected" | sed -e 's/[[:space:]]*$//')"
    fi
    if [ "$output" != "$expected" ]; then
      printf "%s" "$output" > "$expected_path"
      echo "UPDATED $(basename "$test")"
      updated=$((updated + 1))
    else
      echo "UNCHANGED $(basename "$test")"
    fi
    continue
  fi

  expected="$(cat "$expected_path")"
  expected="$(printf "%s" "$expected" | tr -d '\r')"
  expected="$(printf "%s" "$expected" | sed -e 's/[[:space:]]*$//')"

  if [ "$output" != "$expected" ]; then
    echo ""
    echo "FAIL $(basename "$test")"
    echo "Expected:"
    echo "$expected"
    echo "Actual:"
    echo "$output"
    if [ "$WRITE_ACTUAL" -eq 1 ]; then
      printf "%s" "$output" > "$expected_path.actual"
    fi
    failed=$((failed + 1))
  else
    echo "PASS $(basename "$test")"
  fi
done

exit_code=0
if [ "$UPDATE" -eq 1 ]; then
  echo ""
  echo "Updated: $updated / ${#tests[@]}"
elif [ "$failed" -gt 0 ]; then
  echo ""
  echo "Failed: $failed / ${#tests[@]}"
  exit_code=1
else
  echo ""
  echo "All tests passed (${#tests[@]})."
fi

exit "$exit_code"
