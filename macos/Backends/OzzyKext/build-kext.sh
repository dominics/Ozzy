#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build OzzyKext.kext into OUTDIR (unsigned). Fails on the first compile or
# link error. Used by install.command and tests/check-kext-build.sh.
set -euo pipefail
OUT=${1:?usage: build-kext.sh OUTDIR}
cd "$(dirname "$0")/../.."   # macos/

CXX_SOURCES=(
	Backends/OzzyKext/OzzyKext.cpp
	Backends/OzzyKext/KextBus.cpp
	OzzyCore/OzzyEngine.cpp
	Devices/Ploytec/PloytecEngine.cpp
	Devices/Dynacord/DynacordEngine.cpp
	Devices/Dynacord/DynacordRing.cpp
)
C_SOURCES=(
	../common/devices/ploytec/dynacord_pacer.c
	../common/devices/ploytec/dynacord_feedback.c
	../common/devices/ploytec/dynacord_midi.c
)

# The Xcode 27.0 SDK's Kernel headers lost the IOPMDriverAssertion typedefs.
DEFAULT_SDK=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
if [ -n "${OZZY_KEXT_SDK:-}" ]; then SDK=$OZZY_KEXT_SDK
elif [ -d "$DEFAULT_SDK" ]; then SDK=$DEFAULT_SDK
else SDK=$(xcrun --sdk macosx --show-sdk-path); fi
KH="$SDK/System/Library/Frameworks/Kernel.framework/Headers"

if [ -n "${OZZY_KEXT_ARCH:-}" ]; then ARCH=$OZZY_KEXT_ARCH
elif [ "$(uname -m)" = arm64 ]; then ARCH=arm64e
else ARCH=x86_64; fi

COMMON=(-arch "$ARCH" -O2 -DKERNEL -D__KERNEL__ -mkernel -fno-builtin -fno-common
	-Wno-deprecated-declarations -nostdinc -I. -I"$KH")
CXXFLAGS=(-x c++ -std=c++11 -fno-exceptions -fno-rtti -fno-use-cxa-atexit
	-Wno-inconsistent-missing-override "${COMMON[@]}")
CFLAGS=(-x c -std=c11 "${COMMON[@]}")

mkdir -p "$OUT/obj" "$OUT/OzzyKext.kext/Contents/MacOS"
OBJS=()
for SRC in "${CXX_SOURCES[@]}"; do
	O="$OUT/obj/$(basename "${SRC%.*}").o"
	clang++ "${CXXFLAGS[@]}" -c "$SRC" -o "$O"
	OBJS+=("$O")
done
for SRC in ${C_SOURCES[@]+"${C_SOURCES[@]}"}; do
	O="$OUT/obj/$(basename "${SRC%.*}").o"
	clang "${CFLAGS[@]}" -c "$SRC" -o "$O"
	OBJS+=("$O")
done
clang++ -arch "$ARCH" -Xlinker -kext -nostdlib -lkmod -lcc_kext "${OBJS[@]}" \
	-o "$OUT/OzzyKext.kext/Contents/MacOS/OzzyKext"
cp Backends/OzzyKext/Info.plist "$OUT/OzzyKext.kext/Contents/Info.plist"
echo "built $OUT/OzzyKext.kext ($ARCH, $(basename "$SDK"))"
