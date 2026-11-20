#!/usr/bin/env bash
# Download and verify an Ixian Hexagon SDK release into tools/server/.sdk/ (gitignored), for building ixian-server.
#
#   tools/server/scripts/fetch-sdk.sh            # the default version below
#   tools/server/scripts/fetch-sdk.sh 0.2.2      # a given version
#
# The SDK is binary-only for the IQ9 (aarch64); on another machine only its headers are used (mock engine, cross builds).
set -euo pipefail

VERSION="${1:-0.2.2}"
REPO="zetic-ai/IxianHexagon"
NAME="ixian-hexagon-${VERSION}-linux-arm64-qcom-iq9"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${HERE}/.sdk"
mkdir -p "${DEST}"
cd "${DEST}"

if [ -e "${NAME}/include/zetic_llm.h" ]; then
    echo "already present: ${DEST}/${NAME}"
    exit 0
fi

BASE="https://github.com/${REPO}/releases/download/${VERSION}"
echo "downloading ${BASE}/${NAME}.tar.gz"
curl -fsSL -o "${NAME}.tar.gz" "${BASE}/${NAME}.tar.gz"
curl -fsSL -o "${NAME}.tar.gz.sha256" "${BASE}/${NAME}.tar.gz.sha256"
sha256sum -c "${NAME}.tar.gz.sha256"
tar xzf "${NAME}.tar.gz"
rm -f "${NAME}.tar.gz" "${NAME}.tar.gz.sha256"
echo "extracted: ${DEST}/${NAME}"
echo "configure with: cmake -S ${HERE} -B build -DIXIAN_SDK_DIR=${DEST}/${NAME}"
