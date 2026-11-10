#!/bin/sh
# Use an explicitly supplied, workspace-local compiler; no package installation.
set -eu
cd "$(dirname "$0")"
: "${ESBUILD:?Set ESBUILD to the path of esbuild 0.28.2}"
[ "$("$ESBUILD" --version)" = 0.28.2 ]
for pair in 'core-check.ts:core-check-native.js' 'file-service-check.ts:file-service-check.js' 'editor-cli.ts:editor-cli.js'; do
  "$ESBUILD" "${pair%:*}" --bundle --format=iife --platform=neutral --target=es2022 --legal-comments=inline --outfile="${pair#*:}"
done
