#!/usr/bin/env bash
# Builds ixian-server for the IQ9 (cross or native) and packs it: ixian-server-<version>-linux-arm64.tar.gz
#   tools/server/scripts/package.sh [sdk dir]     # cross-compiles on an x86-64 host with g++-aarch64-linux-gnu
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${1:-}"
BUILD="$HERE/build-package"
ARGS=(-S "$HERE" -B "$BUILD" -DIXIAN_SERVER_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release)
[ -n "$SDK" ] && ARGS+=(-DIXIAN_SDK_DIR="$SDK")
if [ "$(uname -m)" != "aarch64" ]; then
    ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$HERE/cmake/aarch64-linux-gnu.toolchain.cmake")
fi
cmake "${ARGS[@]}"
cmake --build "$BUILD" -j
VERSION=$(grep -m1 'project(ixian_hexagon_server VERSION' "$HERE/CMakeLists.txt" | sed -E 's/.*VERSION ([0-9.]+).*/\1/')
DIST="$HERE/ixian-server-$VERSION-linux-arm64"
rm -rf "$DIST"
cmake --install "$BUILD" --prefix "$DIST" > /dev/null
${STRIP:-aarch64-linux-gnu-strip} "$DIST/bin/ixian-server" 2>/dev/null || strip "$DIST/bin/ixian-server" || true
tar -C "$HERE" -czf "$DIST.tar.gz" "$(basename "$DIST")"
sha256sum "$DIST.tar.gz" > "$DIST.tar.gz.sha256"
echo "packed: $DIST.tar.gz"
ls -la "$DIST/bin"
