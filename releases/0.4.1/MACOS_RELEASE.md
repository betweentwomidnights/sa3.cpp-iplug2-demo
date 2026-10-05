# SA3 demo v0.4.1: M4 release check

Use `feature/v0.4.1-release` until v0.4.1 is tagged and sa3.cpp's
`feature/v0.1.1-release` until v0.1.1 is tagged. Update submodules recursively.
Build the same private Metal runtime used by FoundationKeys:

```sh
cd ../sa3.cpp
cmake -S . -B build-plugin-metal-011 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DSA3_BUILD_SAT=ON -DSA3_METAL=ON \
  -DGGML_NATIVE=OFF -DSA3_PRIVATE_GGML=ON
cmake --build build-plugin-metal-011 --parallel 4
ctest --test-dir build-plugin-metal-011 --output-on-failure
cd ../sa3.cpp-iplug2-demo # use your local checkout name
cmake -S . -B build-release-metal-041 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DSA3_BUILD_DIR=../sa3.cpp/build-plugin-metal-011 \
  -DSA3_REQUIRE_PRIVATE_GGML=ON
cmake --build build-release-metal-041 --parallel 4 --target \
  SA3IPlug2Demo-vst3 SA3IPlug2Demo-app SA3ReaperExtension
```

The plugin/app keep the runtime libraries beside their executable inside the
bundle. REAPER keeps them in the `SA3ReaperExtension` folder beside its extension.
Confirm private `libsa3-metal-60f49e09-ggml*.dylib` files and `@loader_path` rpaths.
Test a copied bundle outside the build folder: generate, transform, continue and
cancel; test REAPER and load the demo alongside an older FoundationKeys plugin.

Sign nested dylibs first, then plugin/app bundles and the REAPER extension with
your existing Developer ID Application identity, hardened runtime and timestamp.
Verify signatures with `codesign --verify --deep --strict`. Package with `ditto
-c -k --norsrc`, preserving the REAPER companion directory. Use names
`SA3IPlug2Demo-0.4.1-macos-metal-arm64.zip` and
`SA3ReaperExtension-0.4.1-macos-metal-arm64.zip`. Submit each with your existing
`xcrun notarytool` profile/credentials and `--wait`; require `Accepted` before
publication. Generate hashes after packaging. Keep the Windows assets on the
same release when adding the Mac archives. Report host render results and
archive hashes so release notes can be finalized.
