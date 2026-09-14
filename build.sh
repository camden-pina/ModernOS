#!/usr/bin/env bash

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

export PATH="$HOME/opt/cross/bin:$PATH"

echo "Using cross toolchain:"
echo "  $(command -v x86_64-elf-gcc)"
echo

make "$@"

