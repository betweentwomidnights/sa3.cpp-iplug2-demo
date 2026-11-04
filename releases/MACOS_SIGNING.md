# Signing and notarizing Mac releases

These steps apply to every Mac release. Each version's own guide in `releases/<version>/` covers what
to test.

## In GitHub Actions

`.github/workflows/release-macos.yml` builds on a hosted arm64 runner, then signs, notarizes, staples and
zips, so a release needs no hand-off between Macs. It needs these repository secrets, the same six
sa3.cpp's release uses. The workflow signs only when `MACOS_CERT_P12` and `APPLE_NOTARY_KEY_P8` are set.

| Secret | Value |
| --- | --- |
| `MACOS_CERT_P12` | the Developer ID Application certificate and key, exported as `.p12`, then base64 |
| `MACOS_CERT_PASSWORD` | the password of that `.p12` |
| `APPLE_TEAM_ID` | the team ID, which picks the identity out of the `.p12` |
| `APPLE_NOTARY_KEY_P8` | an App Store Connect API key `.p8`, base64 |
| `APPLE_NOTARY_KEY_ID` | that key's ID |
| `APPLE_NOTARY_ISSUER_ID` | that key's issuer ID |

To run it, dispatch the workflow from the branch to build:

```sh
gh workflow run release-macos.yml --ref main -f sa3_cpp_tag=v0.2.0
```

`sa3_cpp_tag` picks the runtime and defaults to the tag set in the workflow. This is a dry run: it uploads
the zips, `SHA256SUMS-macos` and the check report as a workflow artifact, and its summary has the command
that downloads them. Without the secrets, and on any pull request, the zips are signed ad hoc and named
`-unsigned`. They run on the Mac you test on, once a browser download's quarantine flag is removed.

Test renders on the M4 from that artifact, then upload to the release:

```sh
gh workflow run release-macos.yml --ref main -f sa3_cpp_tag=v0.2.0 -f release_tag=v0.5.0
```

The upload needs the signing secrets, a draft release of that name, and `release_tag` matching the bundles'
version. It replaces any Mac assets already on that draft and leaves the Windows ones alone. It builds again
rather than promoting the artifact you tested, so render once from the draft's zips before publishing.

## By hand

`sign-and-notarize.sh` runs all of the steps below, on the Mac that holds the signing identity. Its
usage comment describes the folder layout it expects, which `stage-macos.sh` produces from a build
configured with `-DSA3_EMBED_DEV_PATHS=OFF`:

```sh
releases/stage-macos.sh <demo-build-dir> <sa3.cpp-dir> <stage-dir>
releases/sign-and-notarize.sh <notary-profile> <stage-dir>
releases/check-macos-release.sh <stage-dir>/signed 13.3
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
