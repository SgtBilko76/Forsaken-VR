#!/usr/bin/env bash
# Builds libpng, zlib and openal-soft for arm64-android with vcpkg into build-android-deps/.
# Needs vcpkg (VCPKG_ROOT or ~/vcpkg) and the Android NDK 27.2.
set -euo pipefail
cd "$(dirname "$0")/../.."
VCPKG="${VCPKG_ROOT:-$HOME/vcpkg}/vcpkg"
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.2.12479018}"
"$VCPKG" install --triplet arm64-android --x-manifest-root=quest/vcpkg --x-install-root=build-android-deps
