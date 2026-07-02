# sa3-iplug2-demo

Small IPlug2 experiment for validating `libsa3` embedded directly in a plugin.

This repo intentionally stays separate from `gary-in-the-plug`. It reuses a few UI/audio-file helpers from that project, but the audio generation path calls `libsa3` in-process rather than going through HTTP.

The demo keeps generated audio at its native `libsa3` sample rate for saving/dragging into a DAW, and creates a separate host-rate playback copy for auditioning inside the plug-in.

## Current scope

- `generate`, `transform`, and `continue` are separate tabs with separate prompt text.
- `generate` creates audio from text only.
- `transform` uses the source audio plus the transform-only init-noise slider.
- `continue` uses the source audio as inpaint context and treats the duration slider as the desired total length.
- The source waveform lives above the render controls, and generated output lives below them with play/stop/save/drag-out support.

## Layout

- `SA3IPlug2Demo/` contains the IPlug2 plugin/app target.
- `SA3IPlug2Demo/DemoAudioFileStore.*` handles WAV/MP3 drop/load and WAV save/drag-out helpers.
- `SA3IPlug2Demo/SA3IPlug2Demo.*` owns the UI, host-input capture, embedded `sa3_context`, and render worker.

## LoRAs

The UI can import `.gguf` LoRA files into `Documents/sa3-iplug2-demo/loras`, enable/remove them, and pass strength sliders through `libsa3` as full-path LoRA entries. `.ckpt` LoRA conversion is intentionally not handled in this demo yet; that helper should live in `libsa3`/`sa3.cpp` so other embedded hosts can use the same conversion path.

The current UI draws the first two imported LoRAs and still keeps additional imported LoRAs queued for renders. This is enough for the demo pass, but a scrollable LoRA list would be the next UI polish step.

## Sample rates

Generated output is stored at the sample rate reported by `libsa3`. `myOutput.wav`, the timestamped drag-out copies, and the output waveform metadata all use that native rate. The plug-in also builds a separate host-rate copy only for internal audition playback, so dragging audio into a DAW should not inherit the standalone app or host device sample rate by accident.

Dropped source audio is decoded from WAV/MP3 and converted to the current host rate before it is captured into the source buffer. Host-recorded source audio is already at the host rate. `libsa3` receives the source buffer with an explicit sample rate for transform/continue.

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

## Backend notes

The CMake defaults prefer `C:/dev/sa3.cpp/build-cuda` when that build contains `sa3.lib` and `ggml-cuda.dll`. Runtime DLLs are copied beside the standalone app and VST3 binary after build, including `sa3.dll`, `ggml*.dll`, and the CUDA runtime DLLs found under `SA3_CUDA_RUNTIME_DIR`.

If the demo feels CPU-bound, confirm that `SA3_BUILD_DIR` points at the CUDA-enabled `sa3.cpp` build and that `ggml-cuda.dll`, `cudart64_*.dll`, `cublas64_*.dll`, and `cublasLt64_*.dll` are present beside the built app/VST3 binary.

The render request uses the same embedded `libsa3` path for all modes. Transform and continue enable outer SAME-L chunked encode/decode settings (`128` with `32` overlap); text generation leaves those chunk settings at zero because there is no init audio to encode. SAME-L sliding-window attention is handled inside `libsa3` from the model metadata, not by a separate plugin UI control.

## DAW scanning notes

On Windows, `SA3IPlug2Demo.vst3` is a bundle directory. It is normal for it to appear as a folder in a file browser; hosts load it from their plug-in browser after scanning.

The VST3/app binaries load `sa3.dll` at render time from beside the binary instead of importing it at module load. This keeps strict host scanners from rejecting the plug-in before the bundled `sa3.dll`, `ggml*.dll`, and CUDA runtime DLLs can be found.

For Ableton testing without admin rights, either scan `C:\dev\sa3-iplug2-demo\build\out` as a VST3 custom folder or copy the bundle to `%LOCALAPPDATA%\Programs\Common\VST3`. Do not point Ableton's VST2 custom folder at the repo/build root, because it will try to scan every helper DLL as a VST2 plug-in.

To refresh the per-user VST3 bundle after a Release build:

```powershell
$userVst3 = Join-Path $env:LOCALAPPDATA 'Programs\Common\VST3'
Copy-Item -LiteralPath C:\dev\sa3-iplug2-demo\build\out\SA3IPlug2Demo.vst3 -Destination $userVst3 -Recurse -Force
```

The current validated metadata is:

- vendor: `the collabage patch`
- version: `0.1.2`
- VST3 validator: `47 tests passed, 0 tests failed`

## Playback crackle triage

Render work runs on a worker thread, not directly in the audio callback. Output audition playback uses a host-rate buffer protected separately from the native output buffer, so waveform redraws and output save/drag work should not block playback as easily.

If standalone playback still crackles on a machine, first test a larger standalone audio buffer or a different ASIO/WASAPI driver. The remaining likely causes are driver/device contention during local testing or GPU/CPU contention while a render is actively running, not the DAW drag/export sample-rate path.
