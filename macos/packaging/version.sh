#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Prints the version a build of this source tree is stamped with (build-pkg.sh, the
# OzzyDaemon Makefile):
#   - Ozzy's own checkout at or past a v* tag: git describe without the v
#     (0.4.1, or 0.4.1-3-gabc1234 three commits on);
#   - Ozzy's own checkout with no v* tag in its history (the trunk: releases are
#     tagged on the release branch's merge commits): the highest v* tag, -dev and
#     the commit (0.4.1-dev+abc1234), or 0.0.0-dev+abc1234 with no v* tag at all;
#   - anything else (git archive, a GitHub ZIP, sources unpacked inside another
#     repository): 0.0.0-dev and the commit common/ozzy_version.txt holds, if any.
# With --build-number it prints the commit count instead (0 outside the checkout).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"

own_checkout() {
	[ "$(git -C "$ROOT" rev-parse --show-toplevel 2>/dev/null)" = "$ROOT" ]
}

if [ "${1:-}" = "--build-number" ]; then
	if own_checkout; then
		git -C "$ROOT" rev-list --count HEAD
	else
		echo 0
	fi
	exit 0
fi

if own_checkout; then
	if tag="$(git -C "$ROOT" describe --tags --match 'v*' 2>/dev/null)"; then
		echo "${tag#v}"
		exit 0
	fi
	# Version order, with a prerelease (v0.4.1-rc.1) below its release
	last="$(git -C "$ROOT" -c versionsort.suffix=- tag -l 'v*' --sort=-v:refname | sed -n 1p)"
	last="${last#v}"
	echo "${last:-0.0.0}-dev+$(git -C "$ROOT" rev-parse --short HEAD)"
	exit 0
fi

sha="$(grep -sv Format "$ROOT/common/ozzy_version.txt" || true)"
echo "0.0.0-dev${sha:++$sha}"
