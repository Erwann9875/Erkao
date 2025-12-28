#!/usr/bin/env bash

set -e

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
chmod 777 "$script_dir"/../*.sh "$script_dir"/*.sh 2>/dev/null || true
