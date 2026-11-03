# crFiles

Standalone Chromium 150 Views file browser. The project has its own Git repository
inside the Chromium source tree and depends on `//libcr` for the shared caption
controls, menu, SVG renderer, themes and Viz compositor.

## Interface

- Thumbnail scheduling coalesces requests into one UI task and refills worker
  slots on completion. Receiving-directory checks run immediately on startup,
  window restoration and preference changes, with a 30-second fallback check.
  Shared-session bubbles reorder only on creation, owner activation or
  visibility changes, foreground/background transitions and anchor changes.
- History creates rows only for the visible region and nearby overscan, with
  lazily measured heights for wrapped paths and search highlights. Unchanged
  History tabs reuse their loaded data and scroll position. More appends 200
  records using a `(time, id)` cursor; a changed journal invalidates pagination
  and reloads the first page instead of mixing database versions.
- On macOS and Windows, Settings → Interface includes an Enable system tray
  checkbox, enabled by default and applied immediately. With it enabled,
  closing the main window hides it while keeping nearby
  discovery and file transfers running. The native menu bar / system tray icon
  toggles the main window's visibility on left-click and has no right-click
  menu. Closing the sending panel keeps
  its transfer running; use its Cancel button to stop the transfer. Owned dialogs
  are hidden without canceling their transfers and restored with the main window. Window objects
  remain in memory. Disabling the tray restores the main window; closing then
  exits normally, as it does if native icon creation fails.
- Pairing, receive consent, and send/receive progress use one independent session
  bubble. When the browser is visible it points to the shared directory or the
  sender's device avatar. With the browser hidden or minimized, it points to the
  native tray icon without restoring the browser. The anchor tracks menu-bar,
  taskbar and display changes, including screen scaling. If the icon is hidden
  or unavailable, the bubble uses the work-area edge and omits its arrow.
  Moving between foreground and background preserves the pairing choices and
  decisions. Completion and errors stay visible until dismissed. The normal
  120-second consent timeout and trusted-device auto-accept preference still
  apply. Nearby sharing must be enabled for background reception.
- The macOS application menu includes About crFiles, Settings (`⌘,`), Hide
  crFiles (`⌘H`), and Quit crFiles (`⌘Q`). Quit and the
  application's quit accelerator explicitly exit and clean up background
  services. Linux retains its existing close-to-exit behavior.
- On macOS, opening a system-recognized package (including `.app` bundles)
  launches its default application. Use **Show Package Contents** in its context
  menu to browse it as a directory. Search results follow the same opening rule.
- The macOS sidebar includes Applications, opening `/Applications`. Application
  bundles in directory views and applications in Open With menus use native
  application icons, loaded on background workers with generic-icon fallback.
- macOS ordinary copy/move operations preserve symbolic links, including dangling
  links, without copying their targets. Copies use `copyfile` to preserve package
  metadata and permissions. Same-volume moves use an atomic rename; cross-volume
  moves retain the source and report partial completion as before. Archive input
  and extraction checks retain their separate link policy.
- macOS defaults to native traffic lights, tabs, and the options menu in a
  single titlebar row. Tabs reserve space for the system controls, without
  custom caption buttons or their separator. Settings → Interface → “Enable windows titlebar style” opts
  into the custom titlebar (off by default; restart crFiles to apply).
- `crFiles.svg` is the window, launcher and Windows executable icon. Regenerate
  the committed Windows ICO with `python3 crFiles/tools/generate_windows_icon.py`
  from the Chromium root (Inkscape and Pillow required).
- The 36-pixel custom frame follows crPlayer, including system/light/dark theme,
  fullscreen, Settings, About and Linux Install on device menu entries.
- macOS uses the system window shadow. Other platforms draw the restored
  window's shadow and 12-pixel rounded corners; maximizing or entering fullscreen
  removes the shadow margins and corner rounding.
- Chrome-style tabs occupy the titlebar and meet the navigation surface.
  Drag tabs horizontally to reorder them without changing the selected page;
  neighboring tabs slide aside and the dragged tab animates into its final slot.
  Tab widths adapt from 184 down to 80 DIPs as space shrinks, with horizontal
  scrolling retained when the minimum widths no longer fit. Order is saved in
  the workspace.
  Unpinned inactive tabs show their close button only while hovered; the active
  unpinned tab keeps it visible. Pinned tabs have no close button. The New Tab
  button follows the last tab when space permits and stays at the right edge
  when tabs overflow, with 2 DIP horizontal margins and a centered 14 DIP icon
  inside a compact 25 DIP button.
  Its background highlights only on hover or press.
  Selecting or adding a tab, and resizing the strip, reveals the active tab
  after layout. Tab scrolling stays synchronized with layout so newly added tabs
  are revealed using the updated content width, without waiting for a compositor
  commit. Tab context
  menus offer New Tab, Pin/Unpin, Close, and Close other tabs. Pinned tabs stay
  ahead of ordinary tabs, can be reordered within their group, and survive Close
  other tabs; their pinned state is saved with the workspace.
  New Tab uses the add icon. In fullscreen, moving to the top edge reveals a circular
  exit-fullscreen button with a slide and fade animation. Each
  tab keeps its directory, navigation history, selected files, view mode and
  scroll position. Tabs overflow horizontally. View options are independent per tab.
- Breadcrumb segments navigate to parent folders; double-clicking the path bar
  opens an editable
  capsule-shaped path field. A segment click starts navigation immediately;
  double-click detection survives segment rebuilding without delaying navigation.
  Enter navigates and restores the breadcrumbs; Escape, focus loss or clicking
  outside the field cancels editing. Relative paths resolve against the active directory.
- Path editing and breadcrumbs crossfade within a fixed capsule over 160 ms.
  After a different directory loads, its viewport fades in and scales from 98%
  to 100% over 160 ms. Loading starts immediately; subsequent navigation or tab
  changes interrupt the previous directory animation.
- The left sidebar resizes by dragging its divider, within 220–360 pixels.
  Ctrl+L toggles the sidebar and divider together without losing their width.
  Its selected location retains a borderless half-capsule highlight independently
  of keyboard focus, and selection follows each tab's current directory.
  Selected locations and bookmarks use the same secondary-container background
  and text colors as Settings navigation.
  Built-in sidebar locations offer Open in New Tab and Properties in their
  context menus. Sidebar and bookmark Properties reuse the file view's layout,
  including the path, type, modification time, and asynchronously calculated
  directory size.
  Desktop uses the system desktop directory, including redirected locations.
  Trash is managed inside crFiles, with Move to Trash and Restore actions.
  Items retain their original paths in per-item metadata. Restore refuses an
  existing destination, and successful restores remove only their empty storage
  directories. Storage is under `$XDG_DATA_HOME/crFiles/Trash` (default
  `~/.local/share/crFiles/Trash`) on Linux, Local AppData on Windows, and
  `~/Library/Application Support/crFiles/Trash` on macOS. This is separate from
  the system trash; system-trash contents are not imported.
- The status bar shares the file content surface: compact action icons on the left,
  item/selection status on the right, with themed hover and press feedback.
  New folder opens an anchored arrow bubble with a name field, separator and
  create button. Names and existing destinations are checked before creation.
  Successful trash/restore operations remove only affected rows without reloading
  the directory, keeping the remaining selection and scroll position.
  New folder opens an anchored arrow bubble with a name field, separator and
  create button. Names and existing destinations are checked before creation.
  Successful trash/restore operations remove only affected rows without reloading
  the directory, keeping the remaining selection and scroll position.
- Back, forward, Up and Refresh use icon buttons. Page buttons provide animated
  hover/focus highlights and click ripples.
- The combined view control toggles list/icon mode on the left and opens its
  options menu on the right. Neither button takes focus. The first menu section
  has a discrete 22 / 32 / 48 / 64 / 96 / 128-pixel icon-size slider, a hidden-files
  checkbox and an expandable Visible Columns section. Sorting is controlled by
  the list headers or the folder background context menu's Sort by submenu. Folders stay before files;
  equal sort keys are resolved by name (reversed for descending Type sorting). Name remains visible; Size, Type and Last
  Modified columns can be toggled. Created and Permissions are also available
  in Visible Columns, both disabled by default; their visibility and widths are
  saved per tab. Single-item Properties shows both fields too. macOS uses birth
  time and POSIX mode bits (including setuid/setgid/sticky flags); Linux uses
  statx birth time where supported. Windows supplies creation time; POSIX
  permissions are unavailable there. Unavailable metadata is shown as a dash. Drag a header divider to resize its column;
  widths are retained per tab and drag updates are coalesced to 16 ms. Dividers
  use a 2 DIP line with the sidebar divider colors and an 8 DIP drag target.
  The Back, Forward, Up and Refresh buttons use circular feedback, centered
  20 DIP icons and 4 DIP padding in 28 DIP buttons.
  Every header toggles ascending/descending sorting and shows an up/down arrow
  on the right of the active sort column. Header text has 6 DIP horizontal padding; vertical padding and inter-column gaps
  are zero; hover/press highlights are rectangular and dividers span the full
  header height. Their drag targets overlap the buttons
  so highlights touch the separator without a gap. List sizes use localized B, KB, MB, GB and larger units.
  The header
  stays 28 pixels tall while the size slider updates icons and row heights in
  both list and icon modes.
  List rows retain their selection background with an animated primary-color
  hover tint and stronger press feedback.
  All columns share row selection, click feedback and double-click opening. Settings → General →
  **Open folders with a single click** also enables single-click folder navigation in file views
  and search results. It defaults to off; modifier-click selection remains available. Narrow list views
  scroll horizontally.
- The folder background context menu contains New Folder, Sort by, Paste,
  Select All, Open in Terminal and Properties, with separators between groups.
  Sort by exposes every saved sort mode and marks the current choice; it works
  in both list and icon views. Select All selects all displayed entries, including
  offscreen rows. Terminal and Properties target the current directory.
- On Windows, file, folder and drive icons prefer the system Shell icon for the
  actual path, including executable icons, shortcut overlays and customized
  folders. Failed queries and generic unknown-file icons fall back to the bundled
  Adwaita SVG selected by MIME type. Transient Shell query or conversion failures
  keep the fallback visible and retry up to three times, after 250 ms, 1 s and
  3 s. Successful retries update the current view without navigation. Changing
  directories cancels pending retries; generic unknown-file icons do not retry.
  In the Windows `/` drive list, uncached drive icons are grouped into one task
  per visible batch and queried sequentially on a dedicated COM STA shared across
  panes and retries. Ordinary file icons and thumbnails retain parallel loading.
  Shell queries run on COM STA workers and produce 1x and 2x images. Path results
  use a 512-entry / 32 MiB pixel LRU. Decoded images additionally share a
  128-entry / 8 MiB LRU keyed by Shell icon index, overlay and requested size.
  New paths still query Shell to preserve custom icons; no COM objects are cached.
  Both levels expire after 30 seconds and validate their source cache epochs.
  Transient
  failures are not cached. A bounded path cache expires after 30 seconds so later
  refreshes can pick up changed associations. Other platforms retain the bundled
  SVG icons and existing macOS application icons. Image, PDF and video thumbnails
  retain priority. Attribution is in `resources/file_icons/COPYRIGHT`.
  Lightweight vector placeholders appear while icons load; system queries and
  SVG rendering run outside the shared cache lock.
  Windows drive-icon diagnostics use the `[crFiles /]` prefix in the default
  `debug.log` next to the executable (or the working directory if that location
  is not writable). They record drive enumeration, Shell results, image-list
  conversion, cache hits, worker dispatch, retries and the applied icon source.
- Drag a rectangle across file content to select multiple entries in either view;
  hold Ctrl to add to the existing selection. Dragging suppresses the normal click
  action and scrolls toward the pointer when it moves outside the visible area.
- Drag a rectangle across file content to select multiple entries in either view;
  hold Ctrl to add to the existing selection. Dragging suppresses the normal click
  action and scrolls toward the pointer when it moves outside the visible area.
- Windows and Linux folder views use matching overlay scrollbars with capsule
  thumbs for both vertical scrolling and horizontal scrolling in List mode.
  Thumb positions follow floating-point content offsets with subpixel layer
  translation, avoiding integer-position stepping. Positions and sizes update
  immediately; hover feedback and scrollbar fade animations remain enabled.
  Scroll-axis pixel snapping is disabled for these thumbs so fractional display
  scaling does not introduce backward steps when their integer bounds advance.
- Icon tiles and list rows share the same selection. Lists show the enabled columns.
  Click a folder/file icon or name to select it; Ctrl-click toggles multiple
  selections without checkboxes. Double-click or press Enter on a focused item
  to open it. Images, including SVG, open in an in-app preview; other files open
  through the operating system. Open also opens a single selected item.
  Size changes update existing icon tiles in place without rebuilding or resorting
  the directory. The slider thumb follows the snapped value immediately, with
  extra edge padding for its highlight. List and Icon modes load image, PDF and video thumbnails asynchronously for visible and nearby entries and
  retains MIME icons when decoding fails. Previews preserve aspect ratio and
  raster EXIF orientation. Decoding is limited to 64 MiB encoded data and
  64 megapixels of decoded raster data; unsupported images report an open error.
  Moving to Trash and restoring operate on selected files or folders.
- Directory enumeration runs on a worker; stale results cannot overwrite another
  tab or a later navigation. Both List and Icon modes virtualize the full directory:
  new row views cover the viewport and one viewport of overscan on each side.
  Existing rows survive up to two viewports beyond either edge, reducing
  repeated construction during short scroll reversals without expanding prefetch.
  Scroll-time row updates keep the full content extent fixed. Up to 32 evicted
  row widgets remain attached but hidden for reuse within the same file layout;
  rebinding refreshes callbacks, selection, text, columns and icons. Focused,
  pressed and dragged rows are retained rather than recycled. A file-specific
  layout manager skips clean row subtrees during ancestor layout passes.
  Each update fills all missing visible rows with names and cached or placeholder
  icons before preparing offscreen rows. Other column cells retain their widths
  and immediately restore cached formatted text when available; uncached detail
  cells initially contain no text. Missing details follow in batches of
  up to 12 rows with a soft 3 ms budget, starting after 16 ms. Visible details
  precede offscreen details. Full tooltips are generated on demand by the outer
  file row; inner buttons and labels do not publish duplicate tooltips. Tooltip
  and ordinary point-event hit testing use the row/column index instead of
  walking all retained rows, with mirrored coordinates for RTL. Headers retain
  normal control targeting, while gaps use the existing background selection
  and drop handling. Area targeting also uses the index, including pointer
  rectangles expanded by fractional scroll transforms. It retains Chromium's
  60% target-area coverage threshold, nearest-center selection, reverse child
  order for ties and center-point fallback; header controls keep normal targeting.
  Reused rows replace old text before becoming visible,
  and hidden panes resume pending details when shown again. Visible work counts against the 16-row / 4 ms
  soft preparation budget but is not deferred by it; only offscreen work waits
  for the next 16 ms tick. Each update evicts at most 32 old rows.
  Viewport callbacks coalesce row and icon scheduling; visible gaps advance
  pending prefetch work to an immediate task outside event/layout dispatch.
  Windows and Linux folder views use critically damped vertical wheel motion,
  preserving velocity across consecutive inputs and moving content and thumb
  together without stopping/restarting the animation on each wheel event.
  New inputs extend the settling deadline; reversing direction clears velocity.
  Wheel motion and thumb rebound use compositor frames, with the
  default timer retained as a fallback without a compositor.
  When the target is a boundary, the viewport snaps once its leading
  edge reaches the first file row or its trailing edge reaches the last file row
  (a whole tile row in icon mode). The vertical thumb then rebounds inward and
  returns over 220 ms without moving content. New input cancels the rebound,
  and reduced-motion settings disable it. Touchpad input keeps its native path, and direct manipulation cancels
  wheel animation.
  Completed buffered ranges update incrementally: overlapping rows keep their
  geometry, and the ordered slot index finds outgoing rows and missing entries.
  Interrupted batches continue filling their gaps. Model, geometry and external
  row changes rebuild the index, while retained focus/drag rows are still checked.
  Offscreen rows nearest the viewport in the scrolling direction are prepared
  first, including after reversing direction. Child ordering preserves the
  longest already-ordered subsequence to avoid moving unchanged rows repeatedly.
  Hidden recycled rows keep their slots in the child order, avoiding moves solely
  to push them to the end. Icon buttons apply their final vertical layout directly,
  avoiding intermediate horizontal text layout. Idle rows skip transparent hover
  background drawing while preserving selection and pressed/hover feedback.
  Each pane caches up to 128 recently used row display records, including lazily
  formatted text and already-rendered thumbnail representations (a 16 MiB pixel
  budget). The cache never retains thumbnail resize sources and is invalidated
  on tab/request/metadata generation changes; image reuse requires matching
  icon size and scale. During continuous scrolling, pending directory reads
  wait for 120 ms of quiet or at most 750 ms from the first pending change,
  apart from existing drag/operation guards.
  Scrolling reaches every entry without a Show more step. Hidden entries are
  excluded until Show Hidden Files is enabled. On Windows, the hidden attribute
  is also respected.
- English source strings plus ja, ko, zh-CN, zh-HK, zh-TW, ar, fr, fr-CA, ru, es, es-419,
  pt-BR and pt-PT resource packs. Change language in Settings, then restart.

| Shortcut | Action |
| --- | --- |
| Ctrl+T / Ctrl+W | New / close tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Ctrl+L | Toggle left sidebar |
| Alt+Left / Alt+Right | Back / forward |
| F5 | Refresh |
| F11 | Toggle fullscreen |
| A–Z in a focused folder view | Select the next file or folder starting with that letter; press again to continue, wrapping at the end |

## Android Quick Share (Linux, Windows and macOS)

Right-click selected files and choose **Quick Share**, below **Sync to** and **Share to**.
The dialog reuses crFiles' device list, code comparison, progress and cancellation
UI, with a separate service backed by `//crQuickShare:protocol`. It does not launch
the standalone crQuickShare application. The existing Share to transport remains
available independently.

Use the same LAN as the Android device. On Linux, opening this dialog requests BLE Fast
Initiation advertising to wake compatible receivers; Bluetooth being unavailable
does not prevent LAN discovery. If a device does not appear, open its Quick Share
receive screen. The dialog also scans BLE independently of LAN availability.
BLE-only devices remain hidden until LAN discovery supplies a usable address. Unnamed advertisements
use the same BLE address's BlueZ name as a display fallback, then a localized generic name.
Advertising and scanning stop during transfers and when the dialog is destroyed.
Windows and macOS use LAN discovery and transport only; BLE remains Linux-only.
Their backends are implemented but compilation and device interoperability are
not yet verified. Windows currently requires local disk paths without reparse
points; UNC paths are unsupported. The standalone crQuickShare app remains Linux-only.

BLE discovery currently supports FEF3 medium V1/V2 fast advertisements and complete
regular advertisements carried directly in service data, containing a V1 point-to-point
connection advertisement and a valid sharing endpoint. Regular advertisements must
match the NearbySharing service hash at both layers. FE2C wakeup advertisements,
UUID-only records and DCT are not listed as receivers. V2 GATT headers (raw or
base64url) matching the NearbySharing bloom filter trigger serial FEF3 advertisement
slot reads. Only validated service hashes and endpoint metadata become device entries.
Each resolution has a 15-second deadline, up to 15 slots, a one-second global cooldown
and 30/60-second per-device failure/success retry intervals. Closing discovery releases
our GATT reference; stale callbacks are discarded. Anonymous fast advertisements
without a GATT header do not trigger speculative connections. BLE discovery still
does not provide cross-network file transport or guarantee a public device name.
Records expire after 120 seconds without an update; at most 128 Bluetooth addresses
are retained. Address changes are deduplicated only with matching endpoint IDs and
salt and metadata-key hash, ignoring only the public-name flag in the header. LAN matching uses the same fields, never just names;
this association is a display hint, not authorization. Scanning never powers on the
adapter or requests pairing. GATT access only reads advertisement slots. Association diagnostics include Bluetooth addresses and endpoint IDs,
but not names, key material or raw advertisement contents.
Compare the full code on both devices before confirming; no Google login or trusted
device bypass is used. Closing the dialog preserves an active transfer; use Cancel
in the session bubble to stop it. Confirmation and cancellation are session-scoped.

This integration sends files and recursively selected folders using Quick Share's
`parent_folder` fields, with a local limit of 100 files after traversal. Empty
folders are omitted; an empty-only selection is rejected. No ZIP conversion is
used. Receiving restores validated relative paths inside an isolated batch,
rejects file/directory conflicts and symbolic links, and renames duplicate files
within the same folder. Android chooses its own receiving directory and may
handle folder structure differently; Pixel and LineageOS interoperability needs
device validation. Unacknowledged delivery is shown separately from confirmed
completion. Wi-Fi Direct and Windows/macOS integration are not included. Linux links both protocol and BLE source targets;
other platforms do not link those targets or display the menu entry.

**Settings → Quick Share** provides an independent enable switch (on by default),
device name and an integrated receive-directory field with a folder picker. Changes
are saved with Apply. The default directory is `~/Downloads/crFiles Received` on Linux.
Reception starts with the main window, without opening the sender dialog, and remains
available while crFiles is running. The standalone application's ten-minute receive
mode is unchanged. Disabling Quick Share shuts down its service and discovery.

Incoming files use the existing confirmation and progress bubbles. Each transfer
requires comparing the code and accepting; the prompt includes file count, size and
save directory. Files are committed into a unique `Transfer-*` subdirectory without
overwriting existing files. The completed bubble's Open action opens that directory
in crFiles, and visible panes showing the destination refresh. URLs and other non-file
attachments remain unsupported. Settings are locked during transfers; backend replacement
waits for idle admission to close, and old session callbacks cannot approve a new session.

Manual reception checks: start crFiles without opening Share, send files from Android,
accept/reject/cancel, verify contents and Open, then change the name and directory.
Disable Quick Share, restart crFiles, verify it stays off, then re-enable it. Check
that configuration remains disabled during transfers and original Share to still works.

Manual verification after building: send a single file and multiple files to
Android; compare codes and verify content; decline or cancel, then send again;
close and reopen the dialog during a transfer; select a folder; and repeat discovery
with Bluetooth unavailable. These checks require a real Android receiver.

## Nearby sharing

Open Nearby devices and enable nearby sharing, or configure it in Settings.
The preference is saved and defaults to off. The app advertises and discovers
`_crphotos._tcp.local` using the same desktop DNS-SD and TLS transport as crPhotos.
Select regular files, then click **Send selected files** on a discovered device.
Compare the entire pairing code on both devices and accept on both ends. The
receiver separately approves the filename/size offer. Cancel is scoped to the
current session. Received batches are placed in separate `Received-*` directories.
When publishing a received file would overwrite a same-name destination, the
receiver asks for **Replace** or **Cancel** after verifying and flushing the new
file. Replace publishes the new file; Cancel keeps the existing file and stops
the remaining transfer. Each conflict requires its own confirmation.

The default receive folder is `~/Downloads/crFiles Received` (the Windows Downloads
known folder is used on Windows). Settings, local identity and trust storage belong
to crFiles, not crPhotos. The settings directory is `~/.config/crFiles` on Linux and
`%LOCALAPPDATA%\crFiles` on Windows. Discovery does not authorize transfers; pairing,
TLS authentication and receive consent are retained from crPhotos.

Desktop interoperability covers Linux and Windows on private IPv4 networks. The
transport also retains the separate Android Connect discovery/client implementation,
but the crFiles panel currently exposes desktop peers only; it does not expose an
Android pairing workflow. Directory transfer and choosing a shared pane require the extended crFiles
protocol on both endpoints. Remote folder browsing and IPv6 are not implemented. See [sharing/README.md](sharing/README.md) for provenance and
manual interoperability checks.

## Build integration

`//crFiles:crFiles` must be listed in `root_extra_deps` in the chosen output's
`args.gn`. It has been added to this workspace's `out/debian` and `out/win10`
configurations. The Chromium root Makefile includes `chromium_src_Makefile`, which
provides `crfiles` and `crfileswin` targets using **autoninja** exclusively.

`make lf` (also `make crfiles`) sets `crfiles_thinlto_optimize_max=false` in
`out/debian/args.gn`, regenerates GN files and builds with the default ThinLTO
link optimization level (O0). `make lf0` sets this argument to `true`, regenerates
GN files, builds with `thinlto_optimize_max` and then packages the result. With
the current official Release configuration and ThinLTO enabled, this selects
link optimization level O2. Both commands preserve the other GN arguments and
explicitly select their mode so a later `make lf` restores O0 after packaging.
Explicit GN generation runs only when the mode changes or `build.ninja` is
missing. Repeating the same mode leaves `args.gn` untouched and proceeds to
autoninja, which still handles regeneration required by other build changes.
Switching modes normally reuses existing C++ objects and runs the ThinLTO link
again. Windows and macOS default to `crfiles_thinlto_optimize_max=true`.

For a future authorized build, run `autoninja -C out/debian crFiles` (Linux) or
`autoninja -C out/win10 crFiles` (Windows). The application accepts folder arguments,
for example `out/debian/crFiles /home/user /tmp --lang=zh-CN`.

No C++ build or application launch was performed during creation, as requested.
Validation was limited to GN syntax formatting, GRIT generation of the header and
12 language packs in `/tmp`, translation/reference audits and transport source
comparison. Runtime layout and two-device interoperability remain to be verified.

## File viewport layout

File scroll views explicitly use layer scrolling with content preferred sizing;
otherwise a plain custom content view can remain at its initial zero size. The
file grid also propagates layout to tile descendants. Scroll restoration waits
for the viewport layout callback instead of racing the initial zero bounds.

The view-controls update was checked against local Chromium 150 interface
signatures, GN syntax, resource generation and translation coverage (76 strings
in 11 translated locales). No application build or runtime UI validation was
performed, in accordance with the workspace instructions.

## Split panes

Each tab retains an independent binary split layout. The four footer controls
create a pane to the left, right, top or bottom of the clicked pane, initially
showing the same directory and view options. Drag the divider to resize panes.
Clicking a pane activates it; the shared breadcrumbs, editable path, history and
view-options controls operate on that pane. Selection, scroll position, sorting,
column widths and directory requests remain local to each pane.
Cmd+W (Ctrl+W on other platforms) closes the active split when a tab has multiple
panes; with a single pane it retains the existing tab-close behavior.

The Show more menu contains Rename pane, Close pane and Move to Trash. Closing
a pane promotes its sibling (including any nested splits); the final pane omits
the close command. Move to Trash is disabled without an eligible selection.
Narrow footers allow horizontal scrolling of the left actions. The action scroll
view uses layer scrolling to size its contents from the buttons' preferred size.
The New folder popover uses a horizontal name field, separator and create button.
Only the window shell owns the nearby-sharing service, so splitting does not
start additional discovery listeners. Resource references and all 11 translation
bundles were checked statically; this change has not been compiled or run.

## Saved workspaces, sharing and drag and drop

Pane names, stable UUIDs, directory paths, split orientation/ratios, active tabs
and panes, view options, column widths, sidebar state and scroll positions are
saved in the crFiles settings workspace. Normal startup restores the workspace;
explicit folder arguments open those folders instead. Invalid or missing paths
show a read error; missing shared paths are not silently redirected elsewhere.

The footer Share toggle advertises a named receive target through the existing
mDNS device service and its TLS identity catalog. Shares are registered by directory
in the main window, with one stable target ID per path. All tabs and splits showing
the same directory display the same toggle state; disabling it in any pane withdraws
that directory's target and cancels a transfer to it. Navigating elsewhere updates
the toggle for the new path, and closing a tab or pane does not revoke its shares.
The target name is captured when sharing starts. Workspace restoration restores
the directory registry, migrating and deduplicating older per-pane share records.
The Nearby panel lists each remote device's shared directories as explicit send targets.
The titlebar menu's Sharing folders entry, below Trusted devices, lists local
shared directories. Remove stops sharing without deleting files; the folder icon
closes the list and opens that directory in a new tab. The list updates after
removal and shows an empty state when no folders remain shared.
Transfers to an explicit shared target write directly into that directory;
transfers to Received use isolated Received-* batch folders. Pairing and receive
approval still apply. Catalogs refresh on
identity probes (normally within 30 seconds), and revoked targets are checked at
transfer time rather than relying on stale discovery records.

Drag an item or existing selection into another pane to copy it, preserving the source files. Native drags offer copy and link operations without offering move. Dragging empty space continues to select items with a rectangle. Drops
highlight the target and run file IO on a worker. Existing destination names are
skipped, never merged or overwritten. Both successful and failed drops leave the
source files intact.
Symbolic links, special files and transfers into the source's own subtree are
rejected. Local operation errors are reported in the pane status bar.

These changes were checked against local Views interfaces and string resources.
No project compilation, GUI execution or network interoperability run was done.

Pane activation ignores standalone modifier keys and shortcut modifier events;
mouse activation also moves keyboard focus to the activated file viewport.
Footers float 2 px above the bottom of the file viewport, using its full width,
rounded themed surfaces and a 160 ms fade/slide. Below 280 px they are hidden.
Activating a pane (including initial restoration) reveals its footer for 8 seconds;
hovering anywhere in an active or inactive pane keeps its footer visible. Open menus/popovers keep it visible
until they close. Footers do not consume layout height. Trailing scroll space lets the final row clear the overlay. Native drags carry a themed image card
with a cached thumbnail/MIME icon, filename and item count. Selected source rows
fade and shrink slightly on pickup, returning when the native drag ends; valid
target outlines fade in and out. The platform owns movement of the drag image.

Drops also resolve child folder rows/tiles in the same viewport. A folder target
gets its own animated outline; blank space targets the current directory. Regular
files and the floating toolbar are not destination folders. Self-drops, drops
into a source's descendants and no-op drops back into its existing parent are
rejected. Destination identity and the viewed directory are captured at drop time
and checked again before background file operations start.

## File operation history

Titlebar → History opens a dedicated tab, sorted newest first. Each row includes
the operation, trigger (drag and drop, menu/button, or remote client), timestamp,
source, destination and outcome. A centered capsule field filters source and destination paths by literal, case-sensitive substring across the entire database; Show more loads additional matching rows.
The tab is restored with the workspace, and visible history updates after operations.

SQLite history is stored beside the private Trash directory as history.sqlite3
(on Linux: $XDG_DATA_HOME/crFiles/history.sqlite3, falling back to
~/.local/share/crFiles/history.sqlite3). Moves, new folders, trash/restore and
shared file sends/receives commit a pending entry before processing and record
success/failure afterward. An interrupted operation retains its pending entry;
it is never replayed automatically. Failure to create a history entry prevents
that operation from starting when recording for that type is enabled. Disabling
a History type suppresses new journal rows without blocking the operation. A successful copy with failed source removal is
reported explicitly. Network sharing remains send/receive, not a destructive move.

Settings uses General, Nearby sharing, History and About pages with the crPlayer
navigation layout. General contains language and theme controls, along with
checkboxes for toolbar hiding, folder animations, hover image previews and
single-click folder navigation.
Nearby sharing groups device avatars, name, receive directory with a folder picker,
discovery and trusted-device acceptance controls; Apply saves and activates changes.
The avatar picker matches crPhotos: four columns, nine circular presets and a
custom-image entry (PNG/JPEG/WebP). Custom avatars use the same base64-encoded
96×96 RGBA format, persist after Apply, and reload on the next visit. Image decoding
is asynchronous and canceled when Settings closes. Clicking outside dismisses the
picker without changing the selection. Controls are disabled during a transfer. About includes the project link and author
email (click to copy). History offers 7, 30 (default), 90 or 365 days of retention and recording
switches for Move, Move to Trash, Access, Open File, Send and Receive. These settings
persist in settings.json. Retention removes expired rows at startup, when changed,
and during journal reads/writes (cleanup is throttled to once per hour). Existing
records are retained when an action is unchecked, subject to retention.
About uses the crFiles icon and the optional /tmp/crFiles_buildtime.h build stamp.

Footer hover is sampled by one window-owned timer every 50 ms against the fixed
bounds of each split pane. Switching tabs settles the destination toolbar's
opacity and position before showing the pane and through its first layout;
hover and auto-hide transitions retain their animations. Item and toolbar enter/exit events never
change visibility, preventing child transitions from restarting the animation. Tab close
controls use vector icons with reserved layout space. History hides the sidebar
without changing the saved preference for file tabs. Successful local moves
remove source rows in place; moving into a subfolder of the same view preserves
the list, header, selection and thumbnails instead of reloading the directory.

History keeps its search header outside the scrolling results, debounces typing,
and discards stale queries without replacing the focused input. Footer hover
uses screen pointer sampling so file rows and icon buttons cannot mask it.

## Directory rendering

Directory enumeration returns file metadata without waiting for icon rasterization.
The current rows remain visible while the next directory loads; only the loading
status changes. Completion renders the new directory once. File view updates do
not rebuild the tabs or breadcrumbs; those controls compare their pane/path/title
state before replacing children. The redundant enumeration-time name sort is gone.

One scheduler for the window gives cache lookups their own worker, checking up
to 32 visible paths per batch without decoding. Cache misses are independent
single-file jobs: up to four images/icons, one PDF and one video can run at once.
Images with an encoded size of at least 8 MiB share one image slot, and concurrent
encoded source buffers share a separate 128 MiB admission budget. Ordinary raster
pixels, estimated HEIC buffers and PDF/SVG output pixels share a 320 MiB budget.
HEIC requests larger than that run exclusively in the pixel pool, with an
800 MiB maximum estimate, retaining support for 64 MP HDR images. These limits
exclude codec-private scratch, retained images and GPU allocations; they are not
process RSS caps. Waits observe cancellation. Separate encoded/pixel pools avoid
holding source buffers while waiting for capacity in the same pool. Ordinary
raster and SVG decoders borrow the source buffer instead of copying it.
Workers refill immediately on completion, so slow files no longer delay
already-decoded neighbors.

Each pane keeps candidate queues by visibility and decoder type. Scrolling
retains requests in the overlapping range, prunes outgoing requests and updates
visible/prefetch priority. Only newly entering ranges and newly bound rows need
classification; completed rows are not rescanned on every row boundary. Cache
misses and completed results only reclassify their own path. Large raster
candidates wait in a separate queue so a busy large-image slot does not force
repeated scans. Refreshes, icon retries, model changes and view-size changes
invalidate the relevant queue state.
Jobs outside the current prefetch range receive per-file cancellation; hidden
panes cancel their outstanding jobs too. Skipped or canceled results release
their pending reservations without marking thumbnails as failed or consuming
icon retries. Active jobs retain their concurrency slots until they return;
codec or Shell calls without cooperative cancellation must finish normally.

Visible entries across splits precede overscan. Results coalesce for 24 ms, then
update at most 16 buttons / a 4 ms soft budget per flush; further results wait for
the next 16 ms tick. Pending results are bounded by viewport scheduling and queue
backpressure. This preserves row widgets, selection and scroll position while
reducing scattered redraws. Filesystem cache validation stays on workers.
Result delivery resolves entries through the materialized-position index.
Eviction compacts column cells, visible buttons and layout slots once per batch;
row ordering only invokes reordering for children whose position changed.

Directory/tab changes, closing panes, and switching mode or icon size cancel old
visual work. Cancellation is checked between entries and around image IO/decode
stages; an already executing codec call must finish. Canceled work keeps its slot
until it returns, so rapid navigation does not exceed the concurrency limit. Old
results are rejected by cancellation token and directory request. Failed previews
retain their MIME icon and are not retried until the directory is reloaded.


Sorting keeps a normalized filename key per metadata entry and an index order for
its current sort mode. Size, modification time and MIME type compare their stored
metadata directly. View-mode changes, icon resizing, hidden-file toggles and scrolling reuse the order; replacing metadata, creating entries or changing the sort
mode invalidates it. Incremental removals retain tombstones and skip them.

Directory metadata snapshots use a UI-sequence LRU with three simultaneous limits:
32 directories, 20,000 entries in total, and a 16 MiB estimated storage budget.
Oversized directories remain browsable but are not retained in the cache. Hits
up to 30 seconds old display immediately, then always enumerate again on a worker.
Unchanged results keep the existing rows, selection, scroll and thumbnails, even
if enumeration order changed. Changed results retain selection and images for
unchanged entries. Explicit refresh bypasses the snapshot; the synthetic Trash
listing is not cached. Path-scoped mutation revisions invalidate affected snapshots, including nested
move/restore destinations; overlapping reads are retried before applying stale
metadata. Unrelated History records do not invalidate directory snapshots.

A separate worker-only thumbnail LRU retains at most 1024 successful images and
64 MiB of accounted bitmap/key storage. Its key includes path, size, modification
and creation timestamps, and requested edge. Metadata is checked on lookup and
again after decoding; filesystem reads and decoding never hold its mutex.
Path-scoped writes invalidate affected thumbnails too, and canceled decoding
cannot populate the cache. Hover previews up to 720 px share this budget; larger
image previews bypass the thumbnail cache. These
budgets bound retained cache storage, not images held by live file views or
in-flight decoders. The directory and decoded-image LRUs are process-local and are not workspace data.

Materialized file entries also retain their current display-size thumbnail.
Its ImageSkia representations are reused across paints and display scales;
changing icon size or replacing the source thumbnail rebuilds that display
image. Removing the row releases the display cache with the other pane images.

Folder thumbnails also have a persistent cache of at most 4096 entries / 256 MiB.
It covers images, SVG, PDF and video thumbnails up to 128 px; hover previews
remain memory-only. Cache keys include the path, size, modification/creation
timestamps and requested edge. Bounded pixel records validate dimensions and
a SHA-256 checksum before allocation. Corrupt records are discarded, and misses
fall back to decoding. Writes run on a separate best-effort sequence with at
most 32 pending images (up to 2 MiB of pixel payload), so disk writes do not delay
thumbnail delivery. Pending writes recheck source metadata and mutation epochs.
Writes use atomic replacement and eviction follows recent use. Linux stores records under `$XDG_CACHE_HOME/crFiles/thumbnails-v1` (or
`~/.cache/crFiles/thumbnails-v1`); Windows uses
`%LOCALAPPDATA%/crFiles/Cache/thumbnails-v1`. Cache lookup and IO run on workers.


## Virtual file views

Enable **Preview images on hover** in **Settings → General** (off by default).
The preference is saved and takes effect immediately. Hover over an image entry
in List or Icon mode for about 400 ms to see a rounded
preview, including SVG images. It fits within 360 × 280 display pixels and the
available window space, preserves aspect ratio, and avoids covering the hovered
row. A cached thumbnail appears first when available, followed by a sharper
worker-decoded image. The overlay uses a 120 ms fade, does not take focus or
intercept clicks, and closes on scrolling, dragging, navigation, or pointer exit.
Each window runs at most one hover decode at a time; canceled requests cannot
replace a newer preview.

Visible folder panes watch their directories for external changes on Linux and
Windows. Panes showing the same path share a watcher and directory reads.
Notifications are coalesced before background enumeration; unchanged visible
rows and the list header are retained while changed entries are replaced.
Selection and keyboard anchors follow surviving paths, and scrolling remains
anchored to the previous top item when it still exists. External renames are
treated as a removal and an addition. Automatic updates do not add History
records or replay folder arrival animations.

Hidden tabs release their subscriptions and revalidate when shown. Returning
to the foreground also rechecks visible folders. Updates wait while a pane is
being dragged, has an open editing popover/menu, or is completing a local file
operation. Failed watchers use a five-second polling fallback; the synthetic
Trash view also polls to detect changes inside its per-item storage folders.

The scroll extent comes from the complete filtered, sorted metadata model, not
from the number of child views. List headers retain their own fixed height and
column resize controls; icon grids recompute their columns when a split changes
width. Scroll/layout callbacks coalesce row updates, retaining overlapping views
and removing rows outside the overscan range. Focused, pressed and native drag
source rows are pinned until they can be safely released. Row ordering and
accessibility positions follow the full model. Arrow keys, Home/End and Page
Up/Down can move beyond materialized rows; Shift extends the selection range.

Selection belongs to file paths in the model. Rubber-band selection intersects
logical item bounds, including rows that have already scrolled out of view.
Completed moves and Trash operations remove affected rows and update the virtual
extent while preserving the remaining views and header. Thumbnail scheduling
still prioritizes visible rows, with separate cache, image, PDF and video lanes. Recycled
rows release their image references to the bounded caches, and late worker
results do not attach images to offscreen metadata entries. History retains its
separate database pagination.

Validation for this change was limited to source/API and structural checks;
no compilation, unit tests, GUI run or performance benchmark was performed.


## Startup and directory work scheduling

Workspace restoration retains hidden tabs as validated saved trees. Only the
selected tab materializes its split views and enumerates its folders; switching
to another tab materializes it on demand. Unopened trees remain serializable,
including pane names, IDs, options and scroll positions. Shared directories are
saved separately from tab trees, validated on file workers, and can be advertised
without opening their views. Navigation, sidebar, nearby panel and fullscreen controls
belong to the shell and are no longer constructed inside every split.

The UI-owned directory queue runs at most two reads concurrently and merges
subscribers requesting the same absolute path spelling (after stripping trailing
separators). A pane leaving a directory/tab cancels its subscription; other panes
waiting on that directory keep their subscription. With no subscribers, queued
work is removed and running enumeration stops cooperatively between entries.
An executing filesystem call must finish before cancellation is observed. Hidden
panes resume an unfinished navigation when selected again.

Initial enumeration returns sorted, filtered indices from the worker. Later sort
or hidden-file changes sort metadata snapshots on workers, using cancellable merge
passes. Request tokens reject obsolete sort results when options or metadata
change. UI rendering consumes these indices; it no longer performs the full sort.
Directory reads, cache entries and panes share immutable metadata snapshots.
Selections, deletion markers and visible image state stay private to each pane.
Live updates compare snapshots and build path/position indices on workers; the
UI applies the diff to existing rows, retaining selection, focus and scroll
anchors. Request tokens and model generations reject obsolete comparisons.
Large snapshots and diff indices are also released on workers. Per-pane sorted
indices and final view/layout bookkeeping remain on the UI thread.

Workspace saves keep the existing debounce and serialize JSON/write files on a
background sequence. Superseded queued snapshots are skipped; a shared write
lock prevents older workspace saves from overwriting explicit Settings changes.
The final close snapshot uses a shutdown-blocking task, and save failures are
reported to the live window or logged during shutdown.

Cache validity is independent of History. Scoped mutations cover local transfer
batches, Trash/restore, folder creation and inbound sharing, including error and
partial-success paths. Observed directory ancestors and changed file subtrees get
revision changes; unrelated caches remain valid. Readers created during a mutation
also see it as active. A successful directory read overlapping a write is displayed provisionally and
refreshed in place once per second, so incoming transfers do not block navigation.
Provisional snapshots and image results crossing a mutation are not cached; a
read invalidated by a completed mutation is retried after 250 ms. Registrations use
weak references so expired paths can be reclaimed, and existing cache capacity
limits remain in effect. This tracks in-process operations; external changes are
still checked by directory revalidation and thumbnail metadata checks.

Only source/API and structural checks were performed for these changes. No build,
unit tests, GUI execution or speedup measurements were run.

Windows adaptation
------------------
The Windows build uses Shell association handlers for Open With, including the
system application chooser. Shell calls initialize COM on their worker thread.
New-folder, rename and archive destinations reject Windows device names, invalid
characters, alternate stream names and trailing spaces/periods. Transfers resolve
existing Windows paths through handles before checking destination ancestry.
Reparse-point source trees remain unsupported; failures retain source files.

The sidebar lists available drive letters and updates once per second. File
clipboard interoperability uses CF_HDROP and Preferred DropEffect: Explorer
copies remain copies and cuts are moved by the normal file-operation pipeline.
Virtual Shell items without filesystem paths are not supported. Cache mutation
checks conservatively cover Windows path case aliases without merging keys for
case-sensitive directories.

The build generates crfiles_buildtime.h in the GN output directory, with
SOURCE_DATE_EPOCH support. The old /tmp header is no longer required.
On the Linux cross-build host, `make fmsix` packages existing Windows x64 outputs
as an unsigned `/tmp/crFiles_<MAJOR>.<MINOR>.<PATCH>.<BUILD>_x64.msix`, using
`crFiles/VERSION`. It does not compile. The package identity is `WURUXU.crFiles`
and its publisher is `CN=72E91141-7461-4054-93AE-74652EFFF800`.
It requires `makemsix` and ImageMagick (`magick`) and includes the same resources,
codec runtimes and licenses as `make wf0`. Override the input directory with
`CRFILES_WIN_OUT_DIR` and the output path with `CRFILES_MSIX_PACKAGE`.
The unsigned package is intended for Store submission or subsequent signing;
local installation requires a signature trusted by the destination computer.

On the Linux cross-build host, `make wf0` packages existing out/win10 outputs
without compiling. Override CRFILES_WIN_OUT_DIR and CRFILES_WIN_PACKAGE_ZIP to
choose the source and ZIP destination. The package includes runtime DLLs,
resource packs, the supported translations and bundled SVG file icons.

Windows receive targets still require local drive paths and reject UNC paths,
junctions and symlinks. Windows GUI behavior, clipboard exchange, device discovery
and file transfer require validation on Windows; this adaptation was checked
statically without compiling or running the application.

Windows navigation paths
------------------------
The address bar and breadcrumbs display local paths as `/c/Users` while all
filesystem operations, copied file paths, History records and Shell integration
keep native Windows paths. Both `/c/Users` and `C:\Users` (or `C:/Users`) can be
entered. UNC paths retain their native representation. Linux paths are unchanged.

`/` is a navigation-only drive list. Drive entries open native drive roots; Up
from a drive root returns to `/`. Back/forward, tabs, split panes and workspace
restoration preserve this location. Available drive letters refresh with the
sidebar and are not stored in the directory cache. The virtual root cannot be a
new-folder, paste, drop or sharing destination; drive entries cannot be moved,
renamed, copied, compressed, shared or deleted from this page. Drive properties
do not recursively scan the entire volume.

Collapsible sidebar
-------------------
Drag the sidebar divider down to zero width to hide it. Release below 110 px
to collapse, or at/above 110 px to settle within the 220–360 px expanded range.
Dragging clips the sidebar contents; release uses a 160 ms ease-out animation.
A transparent 6 px edge remains available when collapsed, with a themed hover
indicator and resize cursor. Drag it outward to expand, or double-click it to
restore the previous expanded width. Ctrl+L also toggles that saved width.
Visibility and expanded width are persisted separately. History hides both the
sidebar and its resize edge without changing the saved sidebar preference.

PDF thumbnails
--------------
List and Icon modes preview the first page of PDFs through the bundled PDFium library.
Rendering fits the page on a white background within the existing 128 px thumbnail
limit. It uses the visible-first file-visual queue and shared 64 MiB / 1024-entry
thumbnail LRU, with metadata and mutation checks before caching. PDFium is lazily
initialized, and all PDF API calls and handle cleanup are serialized on workers.
No document scripts or interactive form environment are started.

Inputs retain the preview loader's 64 MiB file limit. Progressive rendering checks
the directory cancellation token and a two-second cooperative time budget;
document/page parsing and individual rendering operations cannot be forcibly
interrupted. Password-protected PDFs requiring a password, damaged documents,
large files and failed/timed-out previews retain their normal file icon. Double
click continues to launch the system default application.

This change was checked against local PDFium APIs without compiling or running
the application. Linux and Windows runtime preview validation remains pending.


Directory search
----------------

Use **Search** in a folder pane's bottom toolbar to open a dedicated search tab
for that folder and its descendants. The capsule input follows the History page
layout. Its filter icon offers **Search by Path** (default),
**Search by Content**, and an unchecked **Hidden File** checkbox after a
separator. Hidden files and directories are excluded unless enabled; Windows
hidden attributes are respected in addition to dot-prefixed names. Toggling this
option preserves ordinary results and their existing rows. When enabled, a
supplemental hidden-only pass runs after the ordinary pass, enumerating folders
but not rereading ordinary file contents. Turning it off filters hidden matches
from the list; turning it back on reuses matches already collected. A running
hidden pass may finish into the bounded result cache even while its results are
hidden. Typing a query or changing file types prepares a new search; Enter starts
it. Switching a filter menu option starts a search immediately when query text
or a query chip is present.

Type an extension followed by Space, for example `.pdf ` or `.jpg `, to create a
removable type chip. Multiple types are alternatives; `.jpg .png Canon` searches
both image types for `Canon` when content mode is selected. Path mode can use
only type chips; content mode also requires a query. Matching is case-insensitive.

Content sources currently include:

- UTF-8 text and UTF-16 text with a byte-order mark.
- Embedded PDF text, through the bundled PDFium library.
- JPEG EXIF, XMP and comments; PNG eXIf, tEXt, zTXt and iTXt;
  WebP EXIF/XMP; and EXIF fields in small TIFF files.
- MP4, M4V and MOV container metadata, including common title, description,
  author, date, camera, location and embedded EXIF fields. Compressed video
  frames are never decoded or searched.
  Both versioned and unversioned metadata containers are supported, including
  Pixel camera fields such as `com.android.manufacturer` and `com.android.model`.
  Movie, track and media creation/modification timestamps are converted to
  `YYYY-MM-DD HH:MM:SS UTC` for matching years or dates, such as `2025`.
- Recognized EXIF values include camera/lens make and model, capture dates,
  author, description, copyright, exposure, ISO, Windows XP text fields and GPS.

This is metadata/text matching, not OCR, image recognition, a geographic query
engine or an Office document parser. Images without matching embedded metadata
will not match content queries. Scanned PDFs without text are not OCR-processed.

A lazily started `--crfiles-search-process` child handles enumeration and parsing
through the generated `crfiles_search.mojom` interface. Tabs share the process;
parsing runs on one worker sequence. Pull-based batches apply backpressure, and
job/generation IDs discard stale results after editing or canceling. The tab
shows a spinning indicator while searching, a pause icon when paused and a check
when complete; canceled and failed searches also have distinct icons.
**Pause/Resume** is an icon on the left of the centered root-path capsule and
is hidden when no task is active. Typing keeps the query editable; only Enter
starts the search and turns the query into a chip in the search
field. Removing that chip stops the search; Escape also cancels
the task. There is no separate Cancel button. Enter starts searches without a
separate Search button. The result folder action has a
rounded background with animated hover and press feedback. Pause/cancel are cooperative: a native
file read or PDF parsing call already in progress must return first. Closing the
last search tab releases the child process. This process separation does not
constitute an OS security sandbox.

The UI keeps at most 10,000 results in a continuously scrolling virtual list.
Only visible rows and roughly three buffer rows on either side have Views;
offscreen rows are released, except for a focused row retained for keyboard
continuity. New batches append to the visible index without rescanning old hits.
List updates are coalesced at 100 ms and progress labels at 250 ms; completion,
pause and cancel statuses update immediately. Highlighted text caches its
RenderText until bounds, font, theme or display scale changes. Path
search shows one line per result, with matching path text highlighted in color
using regular font weight. Content
search shows the filename on the first line and the highlighted matching excerpt
on the second line. Long lines are elided; hover to see the full text. Double-click
a result to open it; files use the system default application. Each result also
provides an **Open containing folder** icon that selects and scrolls to the
result in its parent directory. Restored search tabs retain
their root folder, but do not automatically rerun previous queries.

Limits keep resource usage bounded: 1,000,000 scanned entries, 10,000 queued
folders, 16 MiB per text file, 64 MiB per PDF, and the first 512 PDF pages with a
three-second cooperative extraction budget. Image metadata has 4 MiB encoded
and decoded budgets and a two-second cooperative scan budget. Whole-file TIFF
metadata reading is limited to 4 MiB. Video box traversal is limited to 4,096
boxes, 4 MiB of metadata and two seconds per file. Symlinks, Windows reparse
points and POSIX special files are skipped. The status line reports skipped/partially searched
files and limits; results can therefore be incomplete for unsupported, protected,
unreadable or oversized files. No persistent content index is created.


## HEIC and HEIF thumbnails

List and Icon modes and hover previews recognize `.heic` and `.heif`, including
when the system does not provide an image MIME type. They use the existing
thumbnail cache and file-change invalidation; decoding runs on workers.

macOS uses ImageIO to downsample the primary image and apply its orientation,
then converts to premultiplied sRGB pixels. No external HEIF library is required.
Linux and Windows use libheif, with container rotation and mirroring applied
once. ICC/NCLX color information is respected, and PQ/HLG HDR is tone mapped to
SDR using the same approach as crPhotos. Suitable embedded thumbnails are used
for SDR images; HDR uses the primary image.

Encoded data is limited to 64 MiB, primary and decoded images to 64 megapixels
and 16,384 pixels per dimension. HEIC decodes are serialized separately from
ordinary raster thumbnails, but now shares pixel-memory admission with them.
Cancellation is checked around codec calls and
during HDR conversion; an executing native codec call cannot be interrupted.
Missing codecs or decode failures retain the normal file icon.

- Linux requires system `libheif.so.1` version 1.19.8 or newer with an HEVC
  decoder backend. The Linux ZIP includes the license, not the system library.
- Windows requires app-local `heif.dll` with a built-in HEVC backend and its
  dependencies. `make wf0` validates and packages existing runtime DLLs from
  `CRFILES_HEIF_RUNTIME_DIR`, a shared codec runtime, `OUT/heif-runtime`, or OUT.
  The packager reports an unavailable codec when no runtime is supplied.

## Video thumbnails

List and Icon modes show a static video frame, fitted to the selected icon size.
MP4, MOV, M4V, MKV and WebM are recognized. H.264 uses Chromium's bundled
FFmpeg decoder. HEVC uses the dynamically loaded libde265 software decoder,
including Main/Main10 content, without requiring GPU decoding or an external
`ffmpeg` executable. Other codecs still depend on the bundled decoder selection.

HDR10/PQ and HLG frames are sampled from 8/10/12-bit planar YUV into floating-point
RGB before Chromium's global tone mapping converts them to SDR/sRGB. This keeps
highlights and wide-gamut color information until the final conversion, matching
the software thumbnail approach in crPhotos. Container HDR metadata is passed to
the tone mapper when available; otherwise Chromium's default mapping is used.
Rotation, mirroring and display aspect ratio are preserved. Dolby Vision dynamic
metadata and HDR10+ dynamic metadata are not applied.

HEVC runtime requirements (libde265 1.0.16 or newer):

- Linux: install the maintained system `libde265.so.0` (Debian package
  `libde265-0`). The Linux archive relies on this system library.
- Windows: place `libde265.dll` (or `de265.dll`) and its runtime dependencies next
  to `crFiles.exe`. Use a matching x64 build, preferably using the MSVC runtime.
  `make wf0` includes either DLL when present and reports when neither is found.
  No library is loaded from the current working directory or PATH.

Only public API declarations and their license are vendored. Missing/older
libraries, unsupported profiles and decoder errors retain the normal video icon.

Video cache misses reuse one crFiles helper process, without a window or GPU
context. A Mojo connection returns bounded native N32 pixels directly; no
temporary PNG files, PNG encoding or re-decoding are needed. The helper retires
after 30 idle seconds and restarts lazily after a timeout, cancellation or crash. H.264 seeks near the start; HEVC starts from the initial
parameter sets and decodes the first available frame, preserving aspect ratio, rotation and mirroring. The
helper uses local-file-only custom I/O, at most 64 MiB of reads, one decoder
thread, a 16-megapixel frame limit and a 128 px output edge. Cancellation or an
eight-second deadline terminates the helper. HEVC picture-buffer allocations
are additionally capped at 32 pictures and a conservative 384 MiB budget. This is process isolation, not an
OS security sandbox.

Only one video job runs at a time, independently of cache probes and the
image/PDF workers. Visible rows precede overscan requests;
navigation cancels obsolete work. Successful frames share the existing bounded
64 MiB / 1024-entry thumbnail cache and file-change invalidation. Unsupported,
damaged or timed-out videos keep their file-type icon.

## Navigation appearance

**Settings → Interface → Navigation bar style** offers three radio choices:
**Minimal path** (default), **Underlined path**, and **Raised segments**.
The setting is saved and applied immediately. Minimal paths use subtle separators
and individual hover highlights; underlined paths mark the current folder with
a theme-colored line and use a 140 ms reveal animation. The existing raised breadcrumb style remains available. Saved capsule selections
now select the underlined style.
All styles use theme colors for light/dark mode and retain path editing, horizontal
scrolling for long paths, and the existing transition between breadcrumbs and the
path input. Hover/press feedback does not change segment widths.

## Plain-text thumbnails

In **Icon** mode, sizes **64, 96 and 128** can show a paper-style content preview
for recognized text extensions, including `.conf`, `.md`, `.txt`, `.bat`, `.sh`,
`.ini`, `.json`, `.yaml`, `.yml`, `.log`, `.cfg`, `.toml`, `.csv`, `.xml`,
`.markdown`, `.bash`, `.zsh`, `.cmd` and `.ps1`. List mode and icon sizes of
48 or below keep the normal type icon, even if a text thumbnail is cached.

A worker reads at most the first **16 KiB** and draws at most the first **5 lines**,
with tabs expanded and long lines clipped. UTF-8 and BOM-marked UTF-16 are
supported; binary controls or invalid encoding fall back to the type icon.
Text is drawn without syntax highlighting or Markdown formatting, using basic
glyph rendering with font fallback (not full bidirectional/complex-script layout).
Linux resolves missing glyphs with worker-local Fontconfig queries and reuses
resolved typefaces within each thumbnail; unavailable glyphs use a replacement.
No script is executed. Text previews share the bounded memory cache and file-change
validation, but are never read from or written to the persistent thumbnail cache.

### Windows HEVC packaging

`make wf0` also discovers existing codecs in `out/win10/hevc-runtime` or
crPhotos' `out/win10/heif-runtime`, then falls back to the output directory.
Override this with `make wf0 CRFILES_HEVC_RUNTIME_DIR=/path/to/runtime`; the
shared `HEIF_RUNTIME_DIR` override is also accepted. No compilation or downloads
are performed. The packager validates x64 PE files and the decoder API exports,
follows non-system DLL imports recursively (including MinGW threading runtimes),
and rejects missing dependencies or conflicting DLLs. Only the decoder and its
required DLLs are copied; unrelated `heif.dll` is not included. Runtime license
and source notices are retained under `licenses/hevc-runtime`. The existing
exclusion of `dxcompiler.dll` and `dxil.dll` remains in place.

An explicitly supplied or auto-detected runtime directory must be complete.
If no codec runtime exists, packaging warns and continues without HEVC support.
The decoder must still satisfy crFiles' runtime version check (libde265 >= 1.0.16);
DLL loading and decoding should be verified on Windows.

### Archive extraction

**Compress** creates ZIP files in a separate archive helper process over Mojo.
The parent performs source checks and records History; the helper reads and
compresses the files, reporting processed bytes through IPC. Results are written
to a private temporary directory and published only after success. Helper launch
failures, disconnections and the 30-minute timeout fail the operation without
publishing an incomplete ZIP. After confirming the ZIP name, an anchored bubble
shows a preparation phase while collecting source sizes, then byte-based progress
(or entry-based progress for empty files), the current filename and a Cancel
button. Progress reaches 100% only after the ZIP is finalized and published. The
bubble points to the first selected filename and keeps the completion or failure
result visible until Close is clicked.
Canceling also stops source checks and cleans up the temporary ZIP; canceled
operations are recorded in History.

Right-click a single `.zip`, `.7z`, `.rar`, `.tar`, `.tar.gz`, `.tgz`, or `.tar.xz` file to **Extract here**,
**Extract to same-name folder**, or **Extract to…**. Extraction starts immediately in an anchored progress bubble showing the file
name and extracted bytes, with cancellation support. ZIP and RAR password input
appears only when required. The bubble changes to a completion notice when done;
success notices dismiss after five seconds, while errors remain for review. Operations are recorded in History.

- ZIP: Store/Deflate and traditional ZIP passwords; AES-encrypted ZIP is not supported.
- 7z: formats supported by Chromium's LZMA SDK; encrypted 7z is not supported.
- RAR: single-volume archives, including password-protected archives; multipart RAR is rejected.
- TAR: regular files and directories in classic TAR, USTAR, PAX and GNU long-name archives.
- TAR.GZ / TGZ and TAR.XZ: streamed through Chromium's zlib and LZMA SDK libraries,
  without external commands or an intermediate TAR file. Compression checksums
  are validated before publishing. Standalone `.gz` and `.xz` files are not included.
- TAR symbolic links (including dangling links) and hard links are supported.
  Regular files are written first, then hard-link chains are resolved, and symbolic
  links are created last. Hard-link targets must be regular files inside the
  archive; missing targets and cycles fail extraction. Sparse files and device
  entries are rejected. Windows symbolic links require developer mode or suitable
  privileges; dangling links use the file-link type when the type is unknown.
  Permissions, ownership,
  extended attributes and timestamps are not restored. Entry names must be UTF-8.
  The same-name destination strips the complete suffix: `photos.tar.gz` becomes `photos/`.

Decoding runs in a disposable helper process, not on the UI thread. This helper
uses the existing crFiles process infrastructure and is not an OS-sandboxed
utility service. Entries are written as regular files and directories into a
private staging tree. TAR links are preserved during publication without following
them; special attributes are not restored.
Only successfully decoded contents are published. Existing real directories are
merged; each conflicting entry prompts for Replace all, Replace, Skip, or Cancel.
Replace all applies only to the current extraction. Originals are temporarily
backed up until their replacement is published; failed rollback preserves the
backup and reports its location. Cancellation stops remaining work, cleans staging
data, and keeps files that have already been successfully published. The current limits are 100,000
entries, 16 GiB of extracted file data and 30 minutes of decoding. Solid 7z
and RAR decoding can require additional temporary disk space.
TAR also limits headers to 100,000, extended metadata to 1 MiB per record and
64 MiB in total, and the decoded stream (including padding) to 17 GiB. XZ
decoder allocations larger than 128 MiB are rejected.

The Chromium UnRAR wrapper also needs `patches/unrar-extraction.patch` when
installing crFiles into a fresh Chromium checkout. It exposes sticky extraction
errors and volume status, and fixes UTF-8 conversion of RAR filenames on Linux.
Apply it from the Chromium source root with
`git apply crFiles/patches/unrar-extraction.patch` (the current workspace already
contains these changes). Both Linux and Windows packages include the UnRAR and
LZMA SDK license texts.

## macOS integration and data handling

- Open With lists the applications registered with macOS and launches the chosen
  application. Video thumbnails first use AVFoundation (including system HEVC
  support) inside the existing disposable helper process, with FFmpeg fallback
  for formats the native decoder cannot read. The parent still enforces its
  decode timeout and cancellation policy.
- crFiles Trash is private to crFiles. It is **not Finder's Trash**. Restore and
  Empty Trash affect only crFiles' trash entries; Finder cannot restore them.
- Finder file URLs can be pasted into crFiles. Command+C/V copies and pastes;
  Command+Option+V requests a move (subject to the cross-volume limitation below).
  Command+X/V remains available for internal cut/paste. Command+A selects files,
  Command+click adds to selection, and Command+Delete sends selection to Trash.
  Application shortcuts use Command, while tab cycling remains Control+Tab.
- Built-in Pictures and Videos bookmarks resolve through macOS user-directory
  APIs, including previously saved built-in bookmarks; custom bookmarks remain
  unchanged. Videos normally resolves to ~/Movies.
- Same-filesystem moves use an atomic no-overwrite rename. If atomic movement
  fails, crFiles can publish a copy, but keeps the originals and reports the
  move as incomplete. It does not recursively delete a live directory after
  copying, because other applications may have written new data during the copy.
  Verify the destination before deciding what to do with the retained originals.

## Regression verification (build/run only when authorized)

`crfiles_dialog_unittests` covers deferred bubble destruction and discarding a
queued close after the original bubble has been replaced. These tests need an
explicit build and execution; adding the target does not establish a passing run.

Manual integration checks:

Directory sharing: open the same directory in two tabs and two splits. Enable
Share in one and verify every matching pane highlights and the remote catalog
contains only one target. Disable it from another pane and verify all highlights
clear. Navigate to another directory and back, close the original tab, reorder
tabs during a receive, and restart with unvisited restored tabs; sharing and target
IDs should remain stable. Restore an older workspace with duplicate per-pane
shares, disable the directory before opening a deferred tab, and verify that
opening it and restarting do not resurrect the share. Repeat with a path with a
trailing separator. Explicitly revoking the active receive target should cancel
that transfer, while changing another directory's share should not.

1. Open view options, new-folder, settings avatar, archive, and sharing bubbles.
   Click outside, switch applications, replace a bubble, then close the parent.
   Check for crashes and dangling-pointer errors at exit.
2. Move within a filesystem and verify the original disappears. Move across
   filesystems while another process changes the source or adds a child. Verify
   originals remain, the move is reported incomplete, and existing destination
   files are never overwritten.
3. Start offline, reconnect, switch Wi-Fi, and attach/detach Ethernet. Nearby
   discovery should retry automatically and discard stale peers. Deny network
   access and check the unavailable message; allow it again and verify recovery.
4. Use Open With to select a non-default application. Test a missing application
   and an unreadable file; a failed launch must not be logged as successful.
5. Preview HEVC MP4/MOV, a rotated video, a short video, an unsupported container,
   and corrupt input. Check thumbnail size/orientation and helper timeout behavior.
6. Transfer files in both directions; reject pairing, cancel transfer, disconnect
   a peer, and close/reopen the dialog. Verify received file hashes and that an
   old prompt cannot act on a new session.

### macOS integration regression checks

- Copy files with spaces and non-ASCII names in Finder, then paste into crFiles;
  verify contents, collision handling and that copying leaves the originals.
- Use Command+Option+V on a same-volume disposable file, then test cross-volume
  movement: the latter must retain the source and report partial completion.
- Check Command shortcuts in file views and in an editing text field; text
  editing must not trigger file operations. Control+Tab must still change tabs.
- Reload a profile whose built-in Videos bookmark stored ~/Videos; verify it
  opens the system Movies directory, while custom bookmarks are unchanged.
- Move disposable files to crFiles Trash, restore them, and verify their contents.
  Check restore collisions do not overwrite existing files. Empty crFiles Trash
  and verify that Finder's system Trash remains unchanged.

## Mac App Store

The sandbox/bundle integration and signing inputs use `com.libcr.crfiles`
(team `6JYK2B5HZ3`). See [mac/README.md](mac/README.md) for entitlements,
folder authorization, inherited helpers, packaging commands and the required
signed-build validation. Loose development builds retain their existing data
paths and helper launch behavior. Packaging does not compile or submit the app.

## Linux shared engine build

Set `enable_libcr_shared=true` and include `//crFiles` in `root_extra_deps` to
build a business EXE linked to `libcr.so`. Keep `enable_libcr_shared=false` for
the original static path. Windows and macOS retain their existing build paths.
Use matching source revisions and GN arguments for all consumers of the engine;
this is a same-build C++ ABI, not a version-stable SDK.

File operations, discovery/sharing, search, preview orchestration, application
Mojo implementations, resources and version information stay in the EXE.
PDFium, SQLite, archive libraries, crypto and media implementations are provided
by the engine. HEIC/HEVC retain the existing optional libheif/libde265 runtime
loading behavior; these external libraries are not bundled into libcr.so.

`make lf` and `make lf0` accept `CRFILES_LINUX_OUT_DIR` (default `out/debian`).
`lf0` builds and packages; it detects the EXE's `libcr.so` dependency and copies
and strips the package copy. Keep the matching library beside the EXE so that
both the UI and application subprocesses can load it via `$ORIGIN`.
The crFiles ThinLTO optimization switch affects the business EXE only; it does
not change the aggregate engine's optimization settings.

Adding these engine dependencies/export settings requires an initial engine
rebuild and can also rebuild tools sharing those implementation targets.
Subsequent edits to business sources or VERSION do not themselves rebuild the
engine. Actual compilation, linking and runtime validation are still required;
GN checks only verify the declared build graph.

Quick Share progress uses the current payload filename and introduction index, per-file and total bytes, and completed payload count (including empty files). Transfer speed and remaining time use a recent sampling window that excludes pairing and confirmation. Sent bytes are transport writes, not delivery receipts; received payload completion does not replace the final batch commit.

## Two-way synchronization

Right-click one file or folder and choose **Sync to**, immediately below **Share to**.
Select another crFiles device by its avatar. The popup has one checkbox,
**Update from remote**, unchecked by default. Leave it off for updates from the
creating device A to receiver B, or check it for two-way synchronization. Confirm
with **Sync** and approve continuing access on the receiver.
If the receiver rejects the request, the relationship pauses and the avatar becomes
clickable again. Select it and press **Sync** to retry the same relationship with
the newly selected permission; the receiver must approve again.
The first copy goes into a fresh directory under its configured receive location.
Subsequent changes follow the creator's permissions while both applications
are running. Two-way conflicts are preserved as separate files, and replaced
or deleted files are retained in a local recovery directory.

The titlebar menu's **Syncing** item, below **Sharing folders**, opens a separate
relationship manager styled like **Trusted devices**. Rows show the remote avatar,
start/last-sync times, the selected name and **Read**/**Write** permissions. Both
ends show A's grants to B: Read allows A-to-B updates; Write allows B-to-A updates.
Only A can change them. B's checkboxes are disabled and update from A on connection.
The avatar opens the local root; trailing icons on either end pause/resume, open
recovery files and remove a relationship without deleting files. Removing a
relationship also removes the peer's list entry after an authenticated notification;
offline notifications persist and retry after reconnection. Actual files and recovery
data remain on both devices. Both peers must support the creator-controlled permissions
extension, plus `sync_removal` for proactive removal notifications. The first version supports one-to-one
desktop relationships, whole-file updates, directory watching and periodic reconciliation.
See [sync-v1 protocol, recovery and acceptance steps](sharing/SYNC_PROTOCOL.md) for
limits, conflict behavior, restart recovery, and the checks still requiring a build
and two-device validation.
