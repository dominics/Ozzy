#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Compile every kext source with the installer's kernel flags and fail on
# the first error. Objects go to a temp dir; nothing is linked or signed.
set -euo pipefail
cd "$(dirname "$0")/.."
SDKROOT=${SDKROOT:-$(xcrun --sdk macosx --show-sdk-path)}
KERNEL_HEADERS="$SDKROOT/System/Library/Frameworks/Kernel.framework/Headers"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
for SRC in Backends/OzzyKext/OzzyKext.cpp Backends/OzzyKext/KextBus.cpp OzzyCore/OzzyEngine.cpp Devices/Ploytec/PloytecEngine.cpp; do
	clang++ -x c++ -arch arm64 -std=c++11 -O2 -DKERNEL -D__KERNEL__ -mkernel \
		-fno-builtin -fno-exceptions -fno-rtti -fno-common -fno-use-cxa-atexit \
		-Wno-deprecated-declarations -Wno-inconsistent-missing-override \
		-nostdinc -I. -I"$KERNEL_HEADERS" -c "$SRC" -o "$OUT/$(basename "${SRC%.*}").o"
done
echo "kext sources compile"
