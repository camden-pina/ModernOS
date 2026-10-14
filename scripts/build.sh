#!/usr/bin/env bash

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

export PATH="$HOME/opt/cross/bin:$PATH"

exec make -C "$PROJECT_ROOT" "$@"
