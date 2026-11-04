# SA3 demo v0.5.0: M4 release build

Build from demo `main` and the sa3.cpp `v0.2.0` tag, and update submodules
recursively. This is the private Metal runtime rather than the `build-metal` one
used to check the branch, so use new build folders. Reusing an old one leaves
stale ggml dylibs inside the bundles.

```sh
cd ../sa3.cpp
git fetch origin --tags && git checkout v0.2.0 && git submodule update --init --recursive
cmake -S . -B build-plugin-metal-v020 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DSA3_BUILD_SAT=ON -DSA3_METAL=ON \
  -DGGML_NATIVE=OFF -DSA3_PRIVATE_GGML=ON
cmake --build build-plugin-metal-v020 --parallel 4
ctest --test-dir build-plugin-metal-v020 --output-on-failure
cd ../sa3.cpp-iplug2-demo # use your local checkout name
git fetch origin && git checkout main && git pull && git submodule update --init --recursive
cmake -S . -B build-release-metal-050 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DSA3_BUILD_DIR=../sa3.cpp/build-plugin-metal-v020 \
  -DSA3_REQUIRE_PRIVATE_GGML=ON
cmake --build build-release-metal-050 --parallel 4 --target \
  SA3IPlug2Demo-vst3 SA3IPlug2Demo-app SA3ReaperExtension
```

The plugin and app keep the runtime libraries beside their executable inside the
bundle. REAPER keeps them in the `SA3ReaperExtension` folder beside its extension.
Confirm private `libsa3-metal-b77ae790-ggml*.dylib` files and `@loader_path` rpaths.
The branch check found build-machine rpaths ahead of `@loader_path`, so test a
copied bundle with the build folders renamed or moved, because otherwise the
bundle can load from the build tree and still look fine.

Because this runtime differs from the one checked on the branch, render once in
each of the extension, the standalone and the VST3. These were accepted without
a run on the branch build, so please check them here if you can:

- VST3 in a host: automation window, cursor, snap, and reopening a saved session
- Transform and Continue with lanes
- A render with a non-English LoRA name
- Cmd-click and Control-drag on lane points

Also load the demo alongside an older FoundationKeys plugin. The standalone asks
for access to the Documents folder on first launch, which is expected.

Sign nested dylibs first, then the plugin and app bundles and the REAPER extension
with your existing Developer ID Application identity, hardened runtime and
timestamp. Verify with `codesign --verify --deep --strict`. Package with `ditto -c
-k --norsrc`, preserving the REAPER companion directory. Use the names
`SA3IPlug2Demo-0.5.0-macos-metal-arm64.zip` and
`SA3ReaperExtension-0.5.0-macos-metal-arm64.zip`. Submit each with your existing
`xcrun notarytool` profile and `--wait`, and require `Accepted` before publication.
Write the hashes to `SHA256SUMS-macos` after packaging, and keep the Windows assets
on the same release when adding the Mac archives. Report host render results and
archive hashes so the release notes can be finalized.

On macOS the extension and its `SA3ReaperExtension` folder install to
`~/Library/Application Support/REAPER/UserPlugins`, and the VST3 to
`~/Library/Audio/Plug-Ins/VST3`.
