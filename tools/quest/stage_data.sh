#!/usr/bin/env bash
# Copies the Forsaken game files to the Quest: /sdcard/Forsaken/{data,scripts,configs,pilots}.
# Usage: tools/quest/stage_data.sh [path-to-forsaken-data] [--reset-config]
#   forsaken-data = a checkout of https://github.com/ForsakenX/forsaken-data (default ../forsaken-data)
#   configs/ and pilots/ are only copied when missing on the headset, unless --reset-config.
set -euo pipefail
cd "$(dirname "$0")/../.."
DATA="${1:-../forsaken-data}"
ADB="${ADB:-$HOME/Android/Sdk/platform-tools/adb}"
DEST=/sdcard/Forsaken
[ -d "$DATA/levels" ] || { echo "No game data in $DATA (expected levels/, models/, ...)"; exit 1; }

"$ADB" shell mkdir -p $DEST/savegame $DEST/logs
echo "Pushing game data (~300 MB)..."
"$ADB" shell rm -rf $DEST/data
"$ADB" push "$DATA/." $DEST/data > /dev/null
"$ADB" shell rm -rf $DEST/data/.git
"$ADB" push scripts/. $DEST/scripts > /dev/null
for d in configs pilots; do
	if [ "${2:-}" = "--reset-config" ] || ! "$ADB" shell ls $DEST/$d > /dev/null 2>&1; then
		"$ADB" shell rm -rf $DEST/$d
		"$ADB" push quest/defaults/$d/. $DEST/$d > /dev/null
	fi
done
"$ADB" shell du -sh $DEST
