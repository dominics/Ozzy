#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the kext with the installer's script into a temp dir and fail on any error.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
Backends/OzzyKext/build-kext.sh "$OUT" >/dev/null
echo "kext sources compile"
