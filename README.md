# UE4 ESP Generic — root, zero game-file dependency

Standalone ARM64 Android ESP built from public UE4 struct knowledge and **live process memory** only.
It never reads `libUE4.so` from your device, never unpacks the game APK, and ships no hardcoded
per-version offsets. Every pointer chain is discovered at runtime from the running game process.

## How it works

1. `su` starts the ARM64 `esphelper64` bundled in `assets/`.
2. The helper finds the target PID by package substring in `/proc/*/cmdline`.
3. It parses `/proc/<pid>/maps`, locates the UE4 module and its first executable mapping as `base`.
4. It scans writable memory inside the module range for a slot whose value is a valid UE4 object
   (vtable pointer lands in an executable mapping) with a plausible `ULevel` actor array.
   That slot address minus `base` becomes `GWorld`.
5. `UWorld::PersistentLevel`, `ULevel::Actors`, `APlayerController::AcknowledgedPawn`,
   `PlayerCameraManager::CameraCachePOV`, `AActor::RootComponent` and
   `USceneComponent::ComponentToWorld` are validated by shape, not by hardcoded offsets.
6. Each frame the helper reads the actor array, projects root positions through the camera POV and
   prints `T l t r b team dist cx cy` lines plus an `F` terminator. The Java service draws them.

Camera projection uses `FMinimalViewInfo` layout: location `+0x0`, rotation `+0xC`, FOV `+0x18`.
`FTransform` is size `0x30`, quaternion `+0x0`, translation `+0x10`.

## Build

```bash
export ANDROID_SDK_ROOT=/path/to/Sdk
export ANDROID_NDK_HOME=$SDK/ndk/25.2.9519653
./scripts/build-helper.sh
./gradlew --no-daemon assembleDebug
# app/build/outputs/apk/debug/app-debug.apk
```

`.github/workflows/build.yml` does the same on push/tag/manual dispatch.

## Install

```bash
adb install -r app-debug.apk
```

1. Open the app, tap **1. Grant overlay permission**.
2. Confirm the package name (`com.tencent.ig` global, `com.tencent.tmgp.pubgmhd` CN HD,
   `com.tencent.tmgp.pubgm` CN) and screen size.
3. Start the game, leave it in the lobby.
4. Tap **2. Start ESP**, switch back to the game.
5. **Show log** prints discovery results and helper errors.

## Manual offsets

If auto-discovery fails on a heavily modified build, tick **Manual offsets** and paste key=value
lines into the box. Anything supplied overrides discovery; missing keys are still auto-detected.

```
lib=libUE4.so
GWorld=0xf06b820
UWorld_Level=0x30
ULevel_Actors=0xa0
PC_AckPawn=0x...
PC_CamMgr=0x...
CamMgr_POV=0x...
AActor_Root=0x168
UScene_C2W=0x...
```

All values are module-relative. Add any absolute address you found in a scanner to the module base
reported in the log.

## Notes

- Read-only: `process_vm_readv` against the game process. Nothing is written into the game.
- Root is required for the helper; overlay permission is required on Android 8+.
- Boxes are root-position height estimates (~175 cm) — no skeleton unless you add bone indices.
- Anti-cheat can still flag overlays. Use a test account.
