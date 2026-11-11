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
- Bluetooth access for discovering Quick Share receivers and initiating file transfers.
- Location access for identifying Wi-Fi networks used by Quick Share hotspot upgrades.
- App Groups for Chromium Mojo's Mach-port rendezvous. The group is
  `6JYK2B5HZ3.com.libcr.crfiles.ipc`; it is also the `CrFilesIPCGroup` Info.plist
  value and the prefix passed to `SetBaseBundleIDOverride` in both processes.

This uses the macOS Team-ID-prefixed App Group format, which Apple documents as
not requiring registration on the developer website. It is used for IPC, not a
shared database or keychain group. If switching to a `group.`-prefixed group,
register/provision it and update **both** the entitlement and Info.plist; changing
only one breaks helper startup. See Apple's [App Groups entitlement documentation](https://developer.apple.com/documentation/BundleResources/Entitlements/com.apple.security.application-groups).

No iCloud, Push Notifications, Sign in with Apple, Associated Domains, Network
Extensions, camera or microphone entitlement is added. Network
multicast entitlement is not needed on macOS. The local-network purpose string
and Bonjour service types are in Info.plist, per
[TN3179](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy).
The Bluetooth purpose string is localized in the same `InfoPlist.strings` files.
Quick Share requests Bluetooth access when device discovery is opened.
It also requests location access to identify Wi-Fi networks and connect to a
phone-provided hotspot or Direct group for transfers across different networks. The location
purpose strings are localized in those same files; the main app has the
`com.apple.security.personal-information.location` entitlement. No location
coordinates are requested. CoreWLAN scans, joins and restoration use a background
task and interfaces provided by `CWWiFiClient`, following Apple's
[CoreWLAN sandbox guidance](https://developer.apple.com/documentation/corewlan).
Quick Share does not read the existing network's password or enterprise
credentials from Keychain before switching networks. After releasing its
temporary hotspot connection, it waits for macOS AutoJoin using system-managed
saved credentials. Reconnection depends on system policy and network
availability; returning to the exact previous network is not guaranteed. A
timeout is reported through the existing network-cleanup warning.
The experimental Direct client is enabled by default and joins a phone-owned
group as an ordinary Wi-Fi station, using the same CoreWLAN connection and
cleanup flow. It can replace the Mac's existing Wi-Fi link; it does not create
a Mac P2P group or require an additional entitlement. Phone interoperability
still needs hardware validation. Use `--quick-share-prefer-direct-client` for a
bounded Direct-first request, or `--disable-quick-share-mac-direct-client` to
compare against LAN/Hotspot only. A pending peer offer can select another
medium, and `--quick-share-prefer-hotspot` takes precedence over the Direct
preference. See the [Direct client behavior and capture procedure](../../crQuickShare/README.md#macos-wi-fi-direct-station-client).
The development executable embeds this Info.plist so native privacy diagnostics
can read usage metadata when launched directly. Use the development app launcher
below to test location authorization with an application bundle and foreground
activation. Packaged applications use the bundle's Info.plist and localized
permission descriptions.

The public CoreBluetooth FE2C/local-name broadcast experiment is enabled by
default while the Quick Share device dialog is discovering. No enable flag is
required; `--disable-quick-share-mac-fast-init-experiment` disables it for comparison.
It uses the existing Bluetooth permission; no extra entitlement is required. OS advertising success does not
establish an Android response. See the [experiment and comparison procedure](../../crQuickShare/README.md#macos-fast-initiation-local-name-experiment).

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

## Run a development app in the foreground

After producing the desired binary, quit any existing crFiles instance and run:

```sh
python3 crFiles/tools/run_macos.py out/arm64
```

The launcher does not compile. It packages existing artifacts into
`out/arm64/crFiles.app`, including usage descriptions, translations, resources,
private libraries and the existing helper layout. It uses local ad-hoc signing,
without a provisioning profile, App Sandbox, hardened runtime or network
timestamp service. This mode is for local development. The distribution and
sandbox packaging modes remain separate.

The launcher stages and verifies the replacement first and replaces only a
bundle marked as a development app with the same bundle identifier. It refuses
to replace unrelated or distribution apps and asks you to quit any running
crFiles instance. Launch Services opens the exact `.app` in the foreground;
application arguments are forwarded without shell expansion. Application logs
are printed in the terminal and saved in a temporary directory shown at launch.
Quit crFiles to finish. Ctrl+C stops the launcher's wait without quitting the app.

For packaging and signature checks without opening a GUI:

```sh
python3 crFiles/tools/run_macos.py --prepare-only out/arm64
```

Open nearby Quick Share discovery while the app is active. Location requests
record `bundle_is_app=1` and `application_active=1`. If discovery opens before
activation, `request_deferred` waits for `NSApplicationDidBecomeActiveNotification`;
closing discovery cancels that deferred request. The app does not request
location access at startup. Permission is confirmed only when the state becomes
`services_enabled=1; authorization=authorized_always; allowed=1`; a submitted
request or initial authorization callback alone does not establish a grant.

For repeatable authorization checks across rebuilt binaries, you may select the
same installed development signing identity with `--identity 'Apple Development: ...'`
before the build-directory argument. Ad-hoc signing does not provide a stable
certificate identity across rebuilds. `--output` can select a different bundle
path; keep the same path and signing identity when comparing authorization runs.
The packaging script also exposes this mode as `--development` for a new output
path, independently of the launcher.

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
The image builder copies only the signed app, the `Applications` shortcut and
the required hidden Finder resources into a separate image root. Generated
`.entitlements` and other signing/diagnostic artifacts beside the staged app
remain available for inspection but are excluded from the disk image. The
runtime permissions are embedded in the app's signature and do not require
shipping the separate entitlement input file.

The direct-download app uses hardened runtime with no sandbox or
sandbox-inheritance entitlements and no App Store provisioning profile.
It signs with `DeveloperID.entitlements`, which grants Location resource access
for Quick Share Wi-Fi discovery and hotspot upgrades. Hardened Runtime requires
`com.apple.security.personal-information.location` even without App Sandbox;
the Info.plist purpose strings alone do not allow the authorization prompt.
The packager verifies that this entitlement is present in the final signature
and leaves the generated `.entitlements` beside the app for inspection.
Location Services authorization is still required from the user. See Apple's
[Location entitlement documentation](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.personal-information.location).
The integrated PDF reader relaunches the existing `crFiles Helper` with
`--crfiles-reader --pdf=PATH`. Its PDFium and window run in a separate process
for each open request, using the same executable code as crFiles. No dedicated
crpdf executable is bundled. PDF PAKs live in `Contents/Resources`. App Store
readers use the worker helper's existing sandbox-inheritance signature;
crFiles transfers an implicit folder-access bookmark through an inherited,
unlinked temporary file descriptor. The reader retains the resolved URL until
shutdown so annotations can be saved even after crFiles exits. Direct-download
and development readers do not use sandbox-inheritance entitlements.

`Contents/Helpers/crFiles Helper` is a relative symlink to `../MacOS/crFiles`:
search, archive, video and PDF reader jobs launch separate processes using the same binary
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
