#!/usr/bin/env bash
# Builds the Linux targets:
#   scripts/build-cardputer-zero.sh          the CardputerZero (arm64) in linux/docker's container
#   scripts/build-cardputer-zero.sh --sim    the SDL2 desktop simulator, on this machine
#   scripts/build-cardputer-zero.sh --deb    the CardputerZero, packaged into dist/ as a .deb
# Options:
#   --clean   start from an empty build directory
# Output: linux/build/<cp0|sim>/scheff-zello, and with --deb
#   dist/scheff-zello-cardputer-zero-v<version>_arm64.deb (version from VERSION)
# The simulator needs SDL2, opus, libcurl and pkg-config (Homebrew on macOS).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/../linux"

TARGET=cp0
CLEAN=false
DEB=false

usage() { sed -n '2,10s/^# \{0,1\}//p' "$SCRIPT_DIR/$(basename "$0")"; }

for arg in "$@"; do
	case "$arg" in
		--sim) TARGET=sim ;;
		--clean) CLEAN=true ;;
		--deb) DEB=true ;;
		-h|--help) usage; exit 0 ;;
		*) echo "Unknown option: $arg"; usage; exit 1 ;;
	esac
done

if $DEB && [ "$TARGET" = sim ]; then
	echo "--deb packages the CardputerZero build only (not --sim)."; exit 1
fi

BUILD_DIR="build/$TARGET"
$CLEAN && rm -rf "$BUILD_DIR"

if [ "$TARGET" = sim ]; then
	# Homebrew's curl is keg-only, and a non-Homebrew pkg-config won't search
	# Homebrew's prefix; point pkg-config at both.
	if command -v brew >/dev/null 2>&1; then
		for p in "$(brew --prefix)/lib/pkgconfig" "$(brew --prefix curl 2>/dev/null)/lib/pkgconfig"; do
			[ -d "$p" ] && export PKG_CONFIG_PATH="$p${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
		done
	fi
	nice cmake --preset sim
	nice cmake --build "$BUILD_DIR"
else
	IMAGE=scheff-zello-cp0-build
	docker build --quiet --platform linux/arm64 -t "$IMAGE" docker >/dev/null
	docker run --rm --platform linux/arm64 \
		--user "$(id -u):$(id -g)" -e HOME=/tmp \
		-v "$PWD/..:/work" -w /work/linux \
		"$IMAGE" sh -c "nice cmake --preset cp0 && nice cmake --build $BUILD_DIR$($DEB && echo " && cd $BUILD_DIR && rm -f ./*.deb && cpack -G DEB")"
fi

echo "Built linux/$BUILD_DIR/scheff-zello"
if $DEB; then
	mkdir -p ../dist
	for deb in "$BUILD_DIR"/scheff-zello-cardputer-zero-*_arm64.deb; do
		cp "$deb" ../dist/
		echo "Packaged dist/$(basename "$deb")"
	done
fi
