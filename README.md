# stable-audio-3 medium + small-music embedded in iPlug2 as c++

this is just an example vst for exploring and validating **libsa3** from
[sa3.cpp](https://github.com/betweentwomidnights/sa3.cpp).
it can run either the stable-audio-3 **medium** or **small-music** GGUF model variant in-process.

i don't rly expect to maintain this project much, because we may end up rolling it into an iPlug2 version of
[gary4juce](https://github.com/betweentwomidnights/gary4juce) (aka gary-in-the-plug).

i'm still not sure if i prefer embedding the model directly inside a vst like this, and may instead continue to
use a "companion application" such as
[gary4local](https://github.com/betweentwomidnights/gary-localhost-installer) for communicating with plugins/[extensions](https://github.com/betweentwomidnights/sa3-ableton-extension) inside the DAW. there are
trade-offs to both. and because you need a separate application to train LoRAs anyway, the companion app still
makes a whole lot of sense.

for now this is just a demo. it helped us find gaps in the **libsa3** package and the underlying pipeline.

if you do want to build this project and play with it, here's claude doing his best to tell you how:

---

The one thing worth calling out up front: unlike gary4juce (which talks to a local server over HTTP), this plugin
calls `libsa3` **in-process** — embedding the model directly in the plugin is the whole point of the experiment.
It reuses some UI concepts and audio-file handling from [gary4juce](https://github.com/betweentwomidnights/gary4juce).

## Current scope

- `generate`, `transform`, and `continue` are separate tabs with separate prompt text.
- `generate` creates audio from text only. With `loop` on it generates an exact `4`/`8`/`16`-bar loop (length derived from BPM).
- `transform` uses the source audio plus the transform-only init-noise slider.
- `continue` uses the source audio as inpaint context and treats the duration slider as the desired total length.
- Shared controls: duration/steps sliders; a seed field (`random`, or lock a seed to reproduce a render); a `shift` dropdown for the sampler distribution shift (`LogSNR`/`Flux`/`Full`/`None`, exposed through `libsa3`); and `bpm` + `key`/scale that get appended to the prompt. BPM follows the host tempo in a DAW and is drag-adjustable in the standalone.
- The `models` menu can download `medium` or `small-music`, point at an existing model folder, and switch the active variant when both complete model sets are present.
- LoRAs can be imported and blended (see below).
- The source waveform lives above the render controls with a `save buffer` button; generated output lives below with play/stop + drag-out. Output auto-saves to `myOutput.wav` after each render, so there's no manual output-save button.
- The editor uses a tall `420x944` layout so it can sit beside a DAW timeline without consuming as much horizontal space.

## Layout

- `SA3IPlug2Demo/` contains the IPlug2 plugin/app target.
- `SA3IPlug2Demo/DemoAudioFileStore.*` handles WAV/MP3 drop/load and WAV save/drag-out helpers.
- `SA3IPlug2Demo/SA3IPlug2Demo.*` owns the UI, host-input capture, embedded `sa3_context`, and render worker.

## LoRAs

Paths below use `<sa3.cpp>` for your sa3.cpp checkout (the `SA3_CPP_DIR` you configured — a sibling checkout by default).

The UI can import `.gguf`, `.safetensors`, and `.ckpt` LoRAs into `Documents/sa3-iplug2-demo/loras`, enable/remove them, and pass strength sliders through `libsa3` as full-path LoRA entries. `.gguf` files are copied directly. `.safetensors` imports are converted to gguf **in-process by `libsa3` (`sa3_convert_lora`) — no Python needed** — as long as the matching `.json` metadata sits beside them. `.ckpt` imports use that same in-process conversion once an exported `.safetensors`/`.json` pair is found beside the checkpoint or in the parent LoRA folder.

Producing that `.safetensors`/`.json` pair from a raw `.ckpt` is the one step that still needs Python (a checkpoint is a PyTorch artifact) — run the helper in `sa3.cpp` once per checkpoint:

```powershell
<sa3.cpp>\.venv\Scripts\python.exe <sa3.cpp>\tools\lora_ckpt_export.py --ckpt <sa3.cpp>\loras\kev\kev.ckpt --out <sa3.cpp>\loras\kev
```

The dice button beside the prompt field uses prompts from enabled LoRAs when their prompt sidecars are discoverable. The importer looks for `.txt` prompt files in the selected LoRA folder and matching `<sa3.cpp>/loras/<name>` folder, then falls back to `<sa3.cpp>/prompts/defaults.json` when no active LoRA prompt pool is available.

The current UI draws the first two imported LoRAs and still keeps additional imported LoRAs queued for renders. This is enough for the demo pass, but a scrollable LoRA list would be the next UI polish step.

## Sample rates

Generated output is stored at the sample rate reported by `libsa3`. `myOutput.wav`, the timestamped drag-out copies, and the output waveform metadata all use that native rate. The plug-in also builds a separate host-rate copy only for internal audition playback, so dragging audio into a DAW should not inherit the standalone app or host device sample rate by accident.

Dropped source audio is decoded from WAV/MP3 and converted to the current host rate before it is captured into the source buffer. Host-recorded source audio is already at the host rate. `libsa3` receives the source buffer with an explicit sample rate for transform/continue.

## Build

The easiest path is to clone this demo and `sa3.cpp` as sibling folders. The demo's CMake defaults look for
`../sa3.cpp`, and then prefer `../sa3.cpp/build-cuda` when it exists. The commands below use `C:\dev` on
Windows and `~/dev` on macOS; any parent folder is fine as long as the two repos stay side by side.

### 1. Get the sources

iPlug2 is vendored as a git submodule at `vendor/iPlug2`, pinned to a small fork of the official repo
(`betweentwomidnights/iPlug2` @ `sa3-demo-tempo`) that adds one required VST3 change — it requests the host
tempo/transport/music-time process context so `GetTempo()` works inside a DAW (needed for the BPM/loop features).

```powershell
New-Item -ItemType Directory -Force C:\dev | Out-Null
cd C:\dev
git clone --recurse-submodules https://github.com/betweentwomidnights/sa3.cpp-iplug2-demo.git
git clone --recurse-submodules https://github.com/betweentwomidnights/sa3.cpp.git
```

If you already cloned this demo without submodules:

```powershell
cd C:\dev\sa3.cpp-iplug2-demo
git submodule update --init --recursive
```

Then fetch iPlug2's SDK dependencies (VST3 SDK, NanoVG, etc.) — these are downloaded artifacts, not in git.
Run the standard iPlug2 downloader from **git-bash** (Windows has no `.ps1` variant):

```bash
cd /c/dev/sa3.cpp-iplug2-demo
cd vendor/iPlug2/Dependencies/IPlug && ./download-iplug-sdks.sh
```

### 2. Build sa3.cpp

Build `libsa3` first. On Windows, the default release build for this demo is CUDA:

```cmd
cd C:\dev\sa3.cpp
build.cmd cuda
```

For a Vulkan build instead:

```cmd
cd C:\dev\sa3.cpp
build.cmd vulkan
```

Model files are not required to compile the plugin. The plugin's `models` button can download `medium` or
`small-music` later, but you can also prefill `C:\dev\sa3.cpp\models` with the `sa3.cpp` downloader:

```cmd
cd C:\dev\sa3.cpp
models.cmd --variant medium
models.cmd --variant small-music
```

The default CMake cache expects:

- `sa3.cpp` at a sibling checkout (`../sa3.cpp`); override with `-DSA3_CPP_DIR=...`
- a Release `sa3.cpp` build. It prefers `<sa3.cpp>/build-cuda` when present, then falls back to `<sa3.cpp>/build`.
- CUDA runtime DLLs under `%CUDA_PATH%/bin` when using the CUDA `sa3.cpp` build.

`IPLUG2_DIR` defaults to the bundled `vendor/iPlug2` submodule; override it only if you want a different iPlug2 checkout.

### 3. Configure and build this demo

Run from the demo repo root:

```powershell
cd C:\dev\sa3.cpp-iplug2-demo
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target SA3IPlug2Demo-vst3
```

If `sa3.cpp` is not a sibling checkout, pass both paths explicitly:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DSA3_CPP_DIR=C:\path\to\sa3.cpp -DSA3_BUILD_DIR=C:\path\to\sa3.cpp\build-cuda
```

For a Vulkan plugin bundle, point at the Vulkan `sa3.cpp` build:

```powershell
cmake -S . -B build-vulkan -G "Visual Studio 17 2022" -A x64 -DSA3_BUILD_DIR=C:\dev\sa3.cpp\build-vulkan
cmake --build build-vulkan --config Release --target SA3IPlug2Demo-vst3
```

On macOS/Apple Silicon, build `sa3.cpp` with Metal first, then point this demo at that build:

```bash
mkdir -p ~/dev
cd ~/dev
git clone --recurse-submodules https://github.com/betweentwomidnights/sa3.cpp-iplug2-demo.git
git clone --recurse-submodules https://github.com/betweentwomidnights/sa3.cpp.git
cd sa3.cpp-iplug2-demo/vendor/iPlug2/Dependencies/IPlug && ./download-iplug-sdks.sh
cd ~/dev/sa3.cpp && ./build.sh metal
cd ~/dev/sa3.cpp-iplug2-demo
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSA3_BUILD_DIR=../sa3.cpp/build-metal
cmake --build build --config Release
open build/out/SA3IPlug2Demo.app
```

Set `SA3_MODELS_DIR` at runtime to override the fallback model directory. The in-plugin `models` button can
also download the `medium` or `small-music` GGUF file set into `Documents/sa3-iplug2-demo/models`, point at an
existing folder, and persist that folder plus the selected variant.

## builds & releases

the backend is decided by which `sa3.cpp` build you point the plugin at (`SA3_BUILD_DIR`) — the plugin just bundles
that backend's DLLs:

- **cuda** — nvidia only, fastest; bundles the cuda runtime.
- **vulkan** — runs on any GPU (amd / intel / nvidia), no cuda runtime, slightly slower. `-DSA3_BUILD_DIR=<sa3.cpp>/build-vulkan`.
- **metal** — apple silicon, built on a mac.

right now we ship zips of the **cuda** and **vulkan** windows builds to github releases with a big **"plz help us
figure out what's working"** disclaimer — this is a demo, not a product. the vulkan build is freshly validated and
works, but i can't cleanly A/B it against cuda yet: **the same seed produces a different-but-valid result on each
backend** (the tensor-core matmuls accumulate differently — see sa3.cpp's cross-backend note). so if something sounds
off (e.g. a silence gap), it's genuinely hard to tell whether it's the backend or just the model doing its thing.
bug reports very welcome.

as of `0.3.0`, the same plugin build supports both stable-audio-3 `medium` and `small-music`. the backend zip
only decides which compute backend gets bundled; the model picker/downloader decides which SA3 variant is active.

## Backend notes

The CMake defaults prefer `<sa3.cpp>/build-cuda` when that build contains `sa3.lib` and `ggml-cuda.dll`. Runtime DLLs are copied beside the standalone app and VST3 binary after build. The CUDA runtime DLLs (`cudart`/`cublas`) are bundled **only** when the selected build ships `ggml-cuda.dll` — a vulkan/metal/cpu build produces a clean backend-only bundle.

If the demo feels CPU-bound, confirm that `SA3_BUILD_DIR` points at the CUDA-enabled `sa3.cpp` build and that `ggml-cuda.dll`, `cudart64_*.dll`, `cublas64_*.dll`, and `cublasLt64_*.dll` are present beside the built app/VST3 binary.

The render request uses the same embedded `libsa3` path for all modes. Text generation, transform, and continue all request outer SAME-L chunked decode (`128` with `32` overlap). Transform and continue also request chunked encode for their source audio. SAME-L sliding-window attention is handled inside `libsa3` from the model metadata, not by a separate plugin UI control.

The demo runs renders in frugal/early-free mode (`keep_models = 0`) so long text2music generations can release T5 before sampling, release DiT before decode, and fully release the autoencoder path after decode. The run button becomes a cancel button during active generation, and plugin teardown asks `libsa3` to cancel cooperatively before joining the worker thread.

## DAW scanning notes

On Windows, `SA3IPlug2Demo.vst3` is a bundle directory. It is normal for it to appear as a folder in a file browser; hosts load it from their plug-in browser after scanning.

The VST3/app binaries load `sa3.dll` / `libsa3.dylib` at render time from beside the binary instead of importing it at module load. This keeps strict host scanners from rejecting the plug-in before the bundled `sa3.dll`, `ggml*.dll`, CUDA runtime DLLs, or macOS `libggml*.dylib` files can be found.

For Ableton testing without admin rights, either scan the repo's `build\out` as a VST3 custom folder or copy the bundle to `%LOCALAPPDATA%\Programs\Common\VST3`. Do not point Ableton's VST2 custom folder at the repo/build root, because it will try to scan every helper DLL as a VST2 plug-in.

To refresh the per-user VST3 bundle after a Release build (run from the repo root):

```powershell
$userVst3 = Join-Path $env:LOCALAPPDATA 'Programs\Common\VST3'
Copy-Item -LiteralPath .\build\out\SA3IPlug2Demo.vst3 -Destination $userVst3 -Recurse -Force
```

The current validated metadata is:

- vendor: `the collabage patch`
- version: `0.3.0`
- VST3 validator: `47 tests passed, 0 tests failed`

## Playback crackle triage

Render work runs on a worker thread, not directly in the audio callback. Output audition playback uses a host-rate buffer protected separately from the native output buffer, so waveform redraws and output save/drag work should not block playback as easily.

If standalone playback still crackles on a machine, first test a larger standalone audio buffer or a different ASIO/WASAPI driver. The remaining likely causes are driver/device contention during local testing or GPU/CPU contention while a render is actively running, not the DAW drag/export sample-rate path.

## Credits / upstreams

- This demo embeds `libsa3` from [sa3.cpp](https://github.com/betweentwomidnights/sa3.cpp).
- The plugin is built with iPlug2. This repo vendors a small fork for the demo, and the official upstream is [iPlug2/iPlug2](https://github.com/iPlug2/iPlug2).
- Stable Audio 3 is from Stability AI; the official repository is [Stability-AI/stable-audio-3](https://github.com/Stability-AI/stable-audio-3).
