# Signing and notarizing Mac releases

These steps apply to every Mac release. Each version's own guide in `releases/<version>/` covers what
to build and test.

`sign-and-notarize.sh` runs all of the steps below, on the Mac that holds the signing identity. Its
usage comment describes the folder layout it expects:

```sh
releases/sign-and-notarize.sh <notary-profile> <stage-dir>
```

Before signing, check that `@loader_path` is the only rpath on each bundled dylib. This covers the
VST3, the app and the REAPER folder:

```sh
otool -l <dylib> | grep -A2 LC_RPATH
```

Then, in this order:

1. Sign nested dylibs first, then the plugin and app bundles and the REAPER extension, with the
   Developer ID Application identity, hardened runtime and a secure timestamp. Check each with
   `codesign --verify --deep --strict`.
2. Zip with `ditto -c -k --norsrc --keepParent`, keeping the REAPER companion folder beside its
   extension.
3. Submit each zip with `xcrun notarytool submit --wait`, and stop unless the status is `Accepted`.
4. Staple the `.app` and `.vst3`, then zip them again. Without a stapled ticket, a first launch needs
   an online lookup, so it can be blocked offline. A zip can't be stapled, so staple the bundles inside
   the folder you zipped. Stapling leaves the signature intact and needs no signing identity, so any
   Mac can do it, and the second zip doesn't need notarizing again.

   ```sh
   xcrun stapler staple SA3IPlug2Demo-macOS-arm64/SA3IPlug2Demo.app
   xcrun stapler staple SA3IPlug2Demo-macOS-arm64/SA3IPlug2Demo.vst3
   ```

   The REAPER extension is a bare dylib and can't be stapled, so its zip from step 2 is final.
5. Write `SHA256SUMS-macos` from the final zips, because stapling changes the zip's hash.
6. Extract the final zips to a new folder and check them before uploading:

   ```sh
   spctl -a -vv -t exec SA3IPlug2Demo-macOS-arm64/SA3IPlug2Demo.app
   xcrun stapler validate SA3IPlug2Demo-macOS-arm64/SA3IPlug2Demo.app
   xcrun stapler validate SA3IPlug2Demo-macOS-arm64/SA3IPlug2Demo.vst3
   codesign --verify --check-notarization -R='notarized' SA3ReaperExtension-macOS-arm64/reaper_SA3ReaperExtension.dylib
   ```

   `spctl` should report `Notarized Developer ID` for the app. It rejects a `.vst3` as not an app even
   when it's notarized, so rely on `stapler validate` there.
