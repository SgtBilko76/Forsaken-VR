#!/usr/bin/env bash
# Builds both release APKs into dist/:
#   Forsaken-VR-v<ver>.apk       game data copied to /sdcard/Forsaken/data by the player
#   Forsaken-VR-v<ver>-full.apk  game data inside, unpacked to /sdcard/Forsaken on first start
# Usage: tools/quest/build_full_apk.sh [forsaken-data folder]
#   default: ../forsaken-data, else the ForsakenX download is fetched into build-fulldata/.
set -euo pipefail
cd "$(dirname "$0")/../.."
VER=$(sed -n "s/.*versionName '\(.*\)'.*/\1/p" quest/app/build.gradle)
DATA="${1:-../forsaken-data}"
if [ ! -d "$DATA/levels" ]; then
	mkdir -p build-fulldata
	if [ ! -d build-fulldata/forsaken-data-master/levels ]; then
		echo "Downloading the ForsakenX game data..."
		curl -sfL -o build-fulldata/data.zip https://github.com/ForsakenX/forsaken-data/archive/refs/heads/master.zip
		(cd build-fulldata && unzip -q -o data.zip)
	fi
	DATA=build-fulldata/forsaken-data-master
fi
DATA=$(realpath "$DATA")
mkdir -p dist
cd quest
./gradlew --offline -q assembleRelease
cp app/build/outputs/apk/release/app-release.apk "../dist/Forsaken-VR-v$VER.apk"
./gradlew --offline -q assembleRelease -PgameData="$DATA"
cp app/build/outputs/apk/release/app-release.apk "../dist/Forsaken-VR-v$VER-full.apk"
# Leave the build tree in the small configuration.
./gradlew --offline -q fxAssets
cd ..
FILES=$(find "$DATA" -path "$DATA/.git" -prune -o -type f ! -name README.md ! -name LICENSE -print | wc -l)
PACKED=$(unzip -l "dist/Forsaken-VR-v$VER-full.apk" | grep -c ' assets/fx/data/' || true)
echo "game data files: $FILES, packed: $PACKED"
[ "$FILES" = "$PACKED" ] || { echo "file count mismatch"; exit 1; }
ls -la dist/
