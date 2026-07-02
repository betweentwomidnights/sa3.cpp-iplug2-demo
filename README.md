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
- a Release `sa3.cpp` build at `C:/dev/sa3.cpp/build`

Configure and build with Visual Studio:

```powershell
cmake -S C:\dev\sa3-iplug2-demo -B C:\dev\sa3-iplug2-demo\build -G "Visual Studio 17 2022" -A x64
cmake --build C:\dev\sa3-iplug2-demo\build --config Release
```

Set `SA3_MODELS_DIR` at runtime to override the default model directory.
