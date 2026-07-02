# sa3-iplug2-demo

Small IPlug2 experiment for validating `libsa3` embedded directly in a plugin.

This repo intentionally stays separate from `gary-in-the-plug`. It reuses a few UI/audio-file helpers from that project, but the audio generation path calls `libsa3` in-process rather than going through HTTP.

## Layout

- `SA3IPlug2Demo/` contains the IPlug2 plugin/app target.
- `SA3IPlug2Demo/DemoAudioFileStore.*` handles WAV/MP3 drop/load and WAV save/drag-out helpers.
- `SA3IPlug2Demo/SA3IPlug2Demo.*` owns the UI, host-input capture, embedded `sa3_context`, and render worker.

## Build

The default CMake cache expects:

- IPlug2 at `C:/dev/gary-in-the-plug/vendor/iPlug2`
- `sa3.cpp` at `C:/dev/sa3.cpp`
- a Release `sa3.cpp` build. It prefers `C:/dev/sa3.cpp/build-cuda` when present, then falls back to `C:/dev/sa3.cpp/build`.
- CUDA runtime DLLs under `%CUDA_PATH%/bin` when using the CUDA `sa3.cpp` build.

Configure and build with Visual Studio:

```powershell
cmake -S C:\dev\sa3-iplug2-demo -B C:\dev\sa3-iplug2-demo\build -G "Visual Studio 17 2022" -A x64
cmake --build C:\dev\sa3-iplug2-demo\build --config Release
```

To force a specific `libsa3` build:

```powershell
cmake -S C:\dev\sa3-iplug2-demo -B C:\dev\sa3-iplug2-demo\build -G "Visual Studio 17 2022" -A x64 -DSA3_BUILD_DIR=C:\dev\sa3.cpp\build-cuda
```

Set `SA3_MODELS_DIR` at runtime to override the default model directory.

## DAW scanning notes

On Windows, `SA3IPlug2Demo.vst3` is a bundle directory. It is normal for it to appear as a folder in a file browser; hosts load it from their plug-in browser after scanning.

The VST3/app binaries load `sa3.dll` at render time from beside the binary instead of importing it at module load. This keeps strict host scanners from rejecting the plug-in before the bundled `sa3.dll`, `ggml*.dll`, and CUDA runtime DLLs can be found.

For Ableton testing without admin rights, either scan `C:\dev\sa3-iplug2-demo\build\out` as a VST3 custom folder or copy the bundle to `%LOCALAPPDATA%\Programs\Common\VST3`. Do not point Ableton's VST2 custom folder at the repo/build root, because it will try to scan every helper DLL as a VST2 plug-in.
