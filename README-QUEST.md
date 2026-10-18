# Forsaken VR for Meta Quest 3

A native Meta Quest port of [Forsaken](https://github.com/ForsakenX/forsaken), the 1998
6-degrees-of-freedom shooter, built on the ForsakenX community source port.

- Full stereo 3D: the levels and the title room are all around you, look around freely while flying.
- HUD as a see-through panel in front of the ship, the crosshair lines up with your guns.
- Hits show as a red glow at the edge of your view instead of a flat red screen.
- Touch controllers as a joystick, remappable in the game's own joystick options.
- 72 Hz, runs standalone on the headset (no PC, no WinlatorXR).

## Install

1. Download `Forsaken-VR-v0.1.0-beta.apk` from the
   [releases page](https://github.com/SgtBilko76/Forsaken-VR/releases).
2. Install it with SideQuest or `adb install -r Forsaken-VR-v0.1.0-beta.apk`.
3. Download the game data:
   [forsaken-data (zip)](https://github.com/ForsakenX/forsaken-data/archive/refs/heads/master.zip)
   from ForsakenX (about 185 MB unpacked).
4. Unzip it and copy the CONTENTS of the `forsaken-data-master` folder to the headset as
   `/sdcard/Forsaken/data` (so that `/sdcard/Forsaken/data/levels` exists), e.g. with SideQuest's
   file manager or `adb push forsaken-data-master/. /sdcard/Forsaken/data`.
5. Start **Forsaken VR** from the app library (Unknown Sources). On the first start, allow
   "All files access" and start the game again.

Scripts and default settings come with the APK and are put into `/sdcard/Forsaken` on start.
Your pilots, settings and save games stay there across updates.

## Controls

| Control | Menus | Flying |
|---|---|---|
| Left stick | Move selection | Forward/back, slide left/right |
| Right stick | Move selection | Turn and pitch |
| Right trigger | Select | Fire primary weapon |
| Left trigger | | Fire secondary weapon |
| A / X | Select | A: next primary weapon, X: move down |
| B / Y | Back | B: next secondary weapon, Y: move up |
| Right / left grip | | Roll right / left |
| Left stick click | | Drop mine |
| Right stick click | | Turbo |
| Menu button (left) | Back | In-game menu |
| Both stick clicks, hold 1 s | Recenter | Recenter |

Sit facing forward when you start a level: the ship's nose points where you were looking.

## Building

Needs the Android SDK, NDK 27.2 and CMake 3.22 (see `quest/app/build.gradle`).

    git submodule update --init quest/external/SDL
    tools/quest/build_deps.sh          # OpenAL, libpng, zlib for arm64-android (vcpkg)
    cd quest && ./gradlew assembleRelease

`tools/quest/stage_data.sh` copies a forsaken-data checkout to a USB-connected headset and
`tools/quest/quest_run.sh` builds, installs, launches and takes a screenshot.

## Credits

Forsaken (c) 1998 Probe Entertainment / Acclaim. Source port and game data distribution by the
[ForsakenX](https://github.com/ForsakenX) community. Quest VR port by SgtBilko76.
