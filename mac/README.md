# Mac App Store packaging

The App ID is `com.libcr.crfiles`, in team `6JYK2B5HZ3`. This directory contains
signing inputs, not a provisioning profile or signing key. Nothing here registers
an account resource, signs a build automatically, or uploads to App Store Connect.

## Account and entitlements

Register an explicit macOS App ID matching the bundle identifier. Obtain a Mac
App Store distribution provisioning profile, an Apple Distribution (or Mac App
Distribution) application signing identity, and a Mac Installer Distribution
identity for the upload package. Install the corresponding private keys locally.

`App.entitlements` enables:

- App Sandbox.
- User-selected files, read/write, with persistent app-scoped bookmarks.
- Downloads, read/write, for the default receiving destination.
- Outgoing and incoming network connections for nearby discovery and transfers.
- App Groups for Chromium Mojo's Mach-port rendezvous. The group is
  `6JYK2B5HZ3.com.libcr.crfiles.ipc`; it is also the `CrFilesIPCGroup` Info.plist
  value and the prefix passed to `SetBaseBundleIDOverride` in both processes.

This uses the macOS Team-ID-prefixed App Group format, which Apple documents as
not requiring registration on the developer website. It is used for IPC, not a
shared database or keychain group. If switching to a `group.`-prefixed group,
register/provision it and update **both** the entitlement and Info.plist; changing
only one breaks helper startup. See Apple's [App Groups entitlement documentation](https://developer.apple.com/documentation/BundleResources/Entitlements/com.apple.security.application-groups).

No iCloud, Push Notifications, Sign in with Apple, Associated Domains, Network
Extensions, Bluetooth, camera or microphone entitlement is added. Network
multicast entitlement is not needed on macOS. The local-network purpose string
and the three Bonjour service types are in Info.plist, per
[TN3179](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy).

The helper has exactly the two sandbox entitlements `app-sandbox` and `inherit`.
Do not sign it with the main application's entitlements or launch it independently.
It inherits the parent's sandbox, including its IPC permissions. It is **not** a
more restrictive security boundary than the parent. See Apple's
[sandbox inheritance requirements](https://developer.apple.com/library/archive/documentation/Miscellaneous/Reference/EntitlementKeyReference/Chapters/EnablingAppSandbox.html).

## Runtime changes

- Bundled builds load ICU, paks and file icons from `Contents/Resources`. ANGLE
  libraries are in `Contents/Frameworks`; Chromium's macOS GL initializer now
  recognizes standalone `.app` bundles as well as framework bundles.
- Search, archive and video jobs use `Contents/Helpers/crFiles Helper`, a separate
  copy of the executable with its own signature in App Store packages, or a
  symlink to the main executable in Developer ID packages. A loose development executable
  still launches itself. Missing packaged helpers fail instead of falling back
  to launching the sandboxed UI executable as a worker.
- Authorized folders are saved as app-scoped security-scoped bookmarks, restored
  before preferences/sharing initialization, and kept open for background sharing
  for the process lifetime. Stale bookmarks are refreshed. Bookmark bytes are
  stored in the private container, never logs or command-line arguments.
- Navigating to inaccessible directories prompts with a native folder picker.
  Canceling or selecting an unrelated directory does not authorize the operation.
  Asynchronous navigation replies are generation-checked. Initial pane checks
  run after attachment to the widget. Only one sheet per window is shown; a
  deferred pane can retry access by navigating/refreshing it.
- Typed extraction destinations, transfer destinations, shared folders and
  receiving-directory settings check access to the nearest existing directory.
  They do not create folders until the actual operation. Granting access does
  not override Unix permissions, read-only volumes, or macOS privacy restrictions.
- Finder clipboard selections and native directory selections retain their
  authorized access. IPC requests carry implicit bookmarks to give already
  running helpers access to newly selected resources. Helpers keep resolved URLs
  alive until their job finishes; extraction also receives an open input file.
- Sandboxed preferences, sharing identities, custom Trash/history and thumbnail
  cache use container Application Support/Caches. Unsandboxed development data
  remains where it was. There is no automatic import from a real-home `.config`
  directory. This preserves the custom crFiles Trash, not Finder's Trash.
- Open/Open With/Open in Terminal use NSWorkspace instead of spawning
  `/usr/bin/open`. Worker calls wait at most ten seconds with explicitly allowed
  synchronization; UI callers do not wait for application launch.

## Package a build

The packaging script does not compile. First produce the desired release build
using the project's normal, explicitly authorized build workflow. Recommended
GN configuration: `is_debug=false`, `is_component_build=false`; ensure the chosen
`mac_deployment_target` matches `--minimum-system-version`. Rebuild after the Mojo
interface changes in this patch. Do not package an old binary with the new plist.

Supply a final `.icns` icon, profile and certificate names. The script defaults to
the bundle/team IDs above, derives the marketing version from `crFiles/VERSION`,
and requires a monotonically increasing App Store build number. The default minimum
macOS version is 13.0. The script rejects executables or private dylibs whose
Mach-O deployment target exceeds the declared minimum. The main and helper use
the same input executable. `make pkg` installs the bundle as `/Applications/crFiles.app`
and retains `/tmp/crFiles-store.pkg` as the package filename. Paths below are
examples, not generated artifacts:

```sh
python3 crFiles/tools/package_macos.py \
  --build-dir out/arm64 \
  --output /tmp/crFiles.app \
  --icon /path/to/crFiles.icns \
  --build-number 1 \
  --profile /path/to/crFiles.provisionprofile \
  --identity 'Apple Distribution: YOUR NAME (6JYK2B5HZ3)' \
  --installer-identity '3rd Party Mac Developer Installer: YOUR NAME (6JYK2B5HZ3)' \
  --pkg /tmp/crFiles-store.pkg
```

For an inspectable layout without signing, replace the signing/profile/package
arguments with `--unsigned`. This is **not** a sandbox or App Store test. Use a
valid development profile and Apple Development identity for a runnable local
sandbox test; omit `--pkg`. Distribution builds are validated via Apple's
supported distribution/testing workflow rather than disabling the sandbox.

The script refuses existing output paths, checks explicit App ID/team/expiry,
rejects non-distribution profiles when producing a package, copies resources,
collects private dylib dependencies and relocates references. It signs dylibs,
then the inherited helper, then the main bundle, verifies the bundle and signer
team, and optionally runs productbuild. Signing is inside-out, without `--deep`
signing. Generated main entitlements are left beside the `.app` for inspection.
Unsupported external dependencies fail packaging rather than producing an app
that silently loads code from the build tree.

## Validation still required before submission

No compilation, app launch, sandbox test, signing, or upload is implied by adding
these files. Validate with the actual newly built and signed application:

1. Cold start outside the build directory; localized strings, Retina resources,
   ICU and ANGLE load. Search/video/archive helper launch and Mojo connection
   succeed without sandbox `mach-register`/`mach-lookup` denials.
2. Deny folder access, then grant a specific folder; restart, navigate restored
   tabs, search and generate thumbnails there. Grant another folder after a
   helper has already launched. Move/remove a bookmarked folder and retry.
3. Extract to a new child directory, compress, copy/move, rename, custom Trash
   and restore, Finder clipboard, and Open With; verify cancellation, inaccessible
   destinations and failed launches report errors without destructive fallback.
4. Configure a receiving folder, restart, discover a real peer, send/receive and
   access a shared directory. Test denial and later re-enabling of local-network
   privacy permission. Verify tab closure does not revoke an active shared folder.
5. Verify the final archive's signatures, entitlements, provisioning, deployment
   target, app icon and architecture, then validate it with Apple's upload tools.

Submission also needs App Store Connect metadata, privacy policy/disclosures,
export-compliance answers and review of the privacy manifest against any changes
to the actual linked Chromium/libcr build. This change does not claim to eliminate
private APIs inside Chromium, provide that audit, or guarantee App Review
approval. No fabricated privacy reasons or broad temporary sandbox exceptions
are added to make a failing binary appear compliant.


## Privacy manifest scope

`PrivacyInfo.xcprivacy` is copied into `Contents/Resources` before signing.
The plain helper executable belongs to this same app bundle. No separate helper
bundle is introduced. The declarations are based on the following code paths:

| Category | Reasons | Current use |
| --- | --- | --- |
| File timestamps | C617.1, 3B52.1 | Container cache metadata and user-authorized files: `crfiles_thumbnail_cache.cc`, `crfiles_image_preview.cc`, `crfiles_window.cc`, and archive/search workers. |
| System boot time | 35F9.1 | Chromium monotonic clocks for in-app elapsed time, timers, animation, and search/archive/transfer timeouts. |
| Disk space | E174.1 | `sharing/nearby_sharing.cc` checks `fstatvfs` before receiving files and fails the transfer if space is insufficient. |

No tracking domains or developer-side collected data are declared. The current
sharing implementation sends user-selected content directly to peers, not to a
developer-operated collection or analytics service. This does not mean that no
data leaves the device; file sharing and peer discovery are network features.
Revisit these declarations if adding telemetry, remote crash reporting, a relay,
or another backend that retains user data.

The native macOS dependencies also read system preferences (`AppleActionOnDoubleClick`,
trackpad settings, and accessibility cursor settings) with `NSUserDefaults`.
These are deliberately NOT described using CA92.1, which excludes reading values
written by the system. No fabricated reason is supplied for these macOS paths.
Apple's current required-reason enforcement article lists iOS, iPadOS, tvOS,
visionOS, and watchOS, not native macOS. The manifest records the uses that map to
approved reasons; it is not a claim of exhaustive certification for Chromium or
a manifest suitable for an iOS/Mac Catalyst port. Re-audit the system-preference
paths if Apple's native macOS requirements change.

References: [Required reason APIs](https://developer.apple.com/documentation/bundleresources/describing-use-of-required-reason-api)
and [approved reasons](https://developer.apple.com/documentation/bundleresources/app-privacy-configuration/nsprivacyaccessedapitypes/nsprivacyaccessedapitype).


## Developer ID DMG distribution

`make dmg` signs existing `out/arm64` artifacts using Developer ID Application,
creates `/tmp/crFiles-<version>.dmg` using `crFiles/VERSION`, submits it with `notarytool`, waits for `Accepted`,
and staples and validates the DMG ticket. A final Gatekeeper assessment must
also pass. It does not compile or publish the DMG to a download server.

The staging directory is `/tmp/crFiles-dmg`; the disk image contains `crFiles.app`
and an `Applications` shortcut. The Finder window uses a bundled background,
centered 90-point icons, and a fixed 720 x 440 window. Creating the image uses
Finder automation; allow Terminal (or the invoking app) to control Finder when
macOS prompts. A graphical login session is required. The default staging directory and DMG are
replaced on each `make dmg` invocation. Custom output paths are not deleted.
The App Store `.app` and `.pkg` outputs are separate and remain untouched.

The direct-download app uses hardened runtime with no sandbox or
sandbox-inheritance entitlements and no App Store provisioning profile.
`Contents/Helpers/crFiles Helper` is a relative symlink to `../MacOS/crFiles`:
search, archive and video jobs launch separate processes using the same binary
and its signature. The App Store package retains a separate helper executable
with sandbox-inheritance entitlements. Never sign the DMG helper symlink with a
different identity or entitlements, since that would modify the main executable.
`make dmg` strips debug and local symbols from staged binaries before signing;
the original build outputs are preserved. External symbols remain available for
dynamic linking. The existing bundle initialization and helper lookup remain in use; sandbox-only
bookmark handling is bypassed by the existing runtime checks. macOS privacy
controls still apply. No JIT, unsigned executable memory, or library-validation
exceptions are added without demonstrated need. Verify the hardened app's UI,
GPU rendering and helper jobs before publishing.

```sh
make dmg
make dmg BUILD_NUMBER=2
# Create/sign a DMG without submitting to Apple:
make package-dmg
# Retry submission and stapling of an existing DMG:
make notarize-dmg
# Use credentials already stored by notarytool:
make dmg NOTARY_KEYCHAIN_PROFILE=crFiles-notary
```

Override `DEVELOPER_IDENTITY`, `DMG_STAGE`, or `DMG_OUTPUT` as needed. Install the
Developer ID Application certificate and its private key in the signing keychain;
a Mac App Distribution identity or Developer ID Installer identity is not a
substitute. DMGs do not require an installer signing certificate.

The existing `API_KEY` and `P8_FILE` variables default to the individual API key.
`NOTARY_ISSUER` must remain empty for individual keys. For a team API key, set it
to the real issuer UUID; do not reuse altool's key-ID issuer placeholder.
Submission status is saved next to the DMG as `.notary.json`; rejected
submissions also attempt to download `.notary-log.json`. A failed submission,
staple or Gatekeeper check stops the target rather than reporting success.

See Apple's [notarization guidance](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution)
and [distribution packaging guidance](https://developer.apple.com/documentation/xcode/packaging-mac-software-for-distribution).

Packaging artwork is generated from the project SVG using AppKit (no external
image libraries). To regenerate the white macOS icon and DMG background:

```sh
swift crFiles/tools/generate_macos_artwork.swift crFiles /tmp/crfiles-white.iconset
iconutil -c icns /tmp/crfiles-white.iconset -o crFiles/crFiles.icns
```
