#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRESET="${PRESET:-cp0-cross}"
CONFIGURATION="${CONFIGURATION:-Release}"

SOURCE_VERSION="$(sed -nE 's/^set\(CAMERA_APP_VERSION "([0-9]+\.[0-9]+\.[0-9]+)".*$/\1/p' \
  "${ROOT_DIR}/CMakeLists.txt" | head -n1)"
if [[ -z "${SOURCE_VERSION}" ]]; then
  echo "Failed to read CAMERA_APP_VERSION from CMakeLists.txt" >&2
  exit 1
fi

# Arguments passed by the caller come last so an explicit version can override the source default.
cmake --preset "${PRESET}" -DCAMERA_APP_VERSION="${SOURCE_VERSION}" "$@"
cmake --build --preset "${PRESET}-rel"
cmake --build "${ROOT_DIR}/build/${PRESET}" --config "${CONFIGURATION}" --target package
