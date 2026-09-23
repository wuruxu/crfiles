# crFiles

Standalone Chromium 150 Views file browser. The project has its own Git repository
inside the Chromium source tree and depends on `//libcr` for the shared caption
controls, menu, SVG renderer, themes and Viz compositor.

## Interface

- `crFiles.svg` is the window, launcher and Windows executable icon. Regenerate
  the committed Windows ICO with `python3 crFiles/tools/generate_windows_icon.py`
  from the Chromium root (Inkscape and Pillow required).
- The 36-pixel custom frame follows crPlayer, including system/light/dark theme,
  fullscreen, Settings, About and Linux Install on device menu entries.
- The restored window has 12-pixel rounded corners and a client-drawn shadow;
  maximizing or entering fullscreen removes the shadow margins and corner rounding.
- The restored window has 12-pixel rounded corners and a client-drawn shadow;
  maximizing or entering fullscreen removes the shadow margins and corner rounding.
- Chrome-style tabs occupy the titlebar and meet the navigation surface.
  New Tab uses only the add icon, with no button background. In fullscreen, moving to the top edge reveals a circular
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
  has a discrete 22 / 32 / 48 / 64 / 96 / 128-pixel icon-size slider, followed by sort radio buttons (A-Z, Z-A, newest
  modification, oldest modification, largest size, MIME type), a hidden-files
  checkbox and an expandable Visible Columns section. Folders stay before files;
  equal sort keys are resolved by name. Name remains visible; Size, Type and Last
  Modified columns can be toggled. Drag a header divider to resize its column;
  widths are retained per tab and drag updates are coalesced to 16 ms. The header
  stays 36 pixels tall while the size slider updates icons and row heights in
  both list and icon modes.
  All columns share row selection, click feedback and double-click opening. Narrow list views
  scroll horizontally.
- File and folder icons use the bundled, unmodified Adwaita SVG assets selected
  by MIME type. No system icon directory is scanned at runtime. Workers rasterize
  only the requested size, with a shared memory cache and no SVG IO under the
  cache lock. Attribution and licenses are in `resources/file_icons/COPYRIGHT`.
  Lightweight vector placeholders appear until the SVG icon is ready; PNG, ICO
  and other raster sources are never used for folder or file type icons.
- Drag a rectangle across file content to select multiple entries in either view;
  hold Ctrl to add to the existing selection. Dragging suppresses the normal click
  action and scrolls toward the pointer when it moves outside the visible area.
- Drag a rectangle across file content to select multiple entries in either view;
  hold Ctrl to add to the existing selection. Dragging suppresses the normal click
  action and scrolls toward the pointer when it moves outside the visible area.
- Icon tiles and list rows share the same selection. Lists show the enabled columns.
  Click a folder/file icon or name to select it; Ctrl-click toggles multiple
  selections without checkboxes. Double-click or press Enter on a focused item
  to open it. Images, including SVG, open in an in-app preview; other files open
  through the operating system. Open also opens a single selected item.
  Size changes update existing icon tiles in place without rebuilding or resorting
  the directory. The slider thumb follows the snapped value immediately, with
  extra edge padding for its highlight. Icon mode loads image thumbnails asynchronously for visible and nearby entries and
  retains MIME icons when decoding fails. Previews preserve aspect ratio and
  raster EXIF orientation. Decoding is limited to 64 MiB encoded data and
  64 megapixels of decoded raster data; unsupported images report an open error.
  Moving to Trash and restoring operate on selected files or folders.
- Directory enumeration runs on a worker; stale results cannot overwrite another
  tab or a later navigation. Both List and Icon modes virtualize the full directory:
  only the viewport and one viewport of overscan on each side have row views.
  Scrolling reaches every entry without a Show more step. Hidden entries are
  excluded until Show Hidden Files is enabled. On Windows, the hidden attribute
  is also respected.
- English source strings plus zh-CN, zh-HK, zh-TW, ar, fr, fr-CA, ru, es, es-419,
  pt-BR and pt-PT resource packs. Change language in Settings, then restart.

| Shortcut | Action |
| --- | --- |
| Ctrl+T / Ctrl+W | New / close tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Ctrl+L | Toggle left sidebar |
| Alt+Left / Alt+Right | Back / forward |
| F5 | Refresh |
| F11 | Toggle fullscreen |

## Nearby sharing

Open Nearby devices and enable nearby sharing, or configure it in Settings.
The preference is saved and defaults to off. The app advertises and discovers
`_crphotos._tcp.local` using the same desktop DNS-SD and TLS transport as crPhotos.
Select regular files, then click **Send selected files** on a discovered device.
Compare the entire pairing code on both devices and accept on both ends. The
receiver separately approves the filename/size offer. Cancel is scoped to the
current session. Received batches are placed in separate `Received-*` directories.

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
mDNS device service and its TLS identity catalog. The target directory is captured
when sharing starts and does not follow subsequent navigation. The colored toggle
and tooltip identify its state and receive path. Rename is available in the footer Show more menu. Workspace restoration also restores valid shared targets; closing a pane
or disabling Share withdraws its target and cancels a transfer to that target.
The Nearby panel lists each remote device's shared panes as explicit send targets.
Receipts retain the existing isolated Received-* batch folders under the selected
shared directory. Pairing and receive approval still apply. Catalogs refresh on
identity probes (normally within 30 seconds), and revoked targets are checked at
transfer time rather than relying on stale discovery records.

Drag an item or existing selection into another pane to move it. Dragging empty space continues to select items with a rectangle. Drops
highlight the target and run file IO on a worker. Existing destination names are
skipped, never merged or overwritten. Copy failure leaves the source intact;
failed source removal after a move leaves the published copy and reports failure.
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
navigation layout. General contains language and theme controls, without checkboxes.
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
bounds of each split pane. Item and toolbar enter/exit events never
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

One scheduler for the window allows at most two visual worker jobs, each returning
at most two entries. It prioritizes visible rows across all visible splits before
prefetching up to one viewport above/below. Scrolling and layout changes coalesce
into a 16 ms scheduling request. Cached MIME icons can be reused immediately on
the UI sequence; SVG reads and thumbnail decoding stay on workers. Results update
only the affected buttons, preserving row widgets, selection and scroll position.

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
cannot populate the cache. Large image previews bypass the thumbnail cache. These
budgets bound retained cache storage, not images held by live file views or
in-flight decoders. Both caches are process-local and are not workspace data.


## Virtual file views

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
still prioritizes visible rows, with two worker jobs of two entries each. Recycled
rows release their image references to the bounded caches, and late worker
results do not attach images to offscreen metadata entries. History retains its
separate database pagination.

Validation for this change was limited to source/API and structural checks;
no compilation, unit tests, GUI run or performance benchmark was performed.


## Startup and directory work scheduling

Workspace restoration retains hidden tabs as validated saved trees. Only the
selected tab materializes its split views and enumerates its folders; switching
to another tab materializes it on demand. Unopened trees remain serializable,
including pane names, IDs, options, shared roots and scroll positions. Shared
roots from lazy trees are validated on file workers and can be advertised without
opening their views. Navigation, sidebar, nearby panel and fullscreen controls
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
Metadata snapshot copies and final UI bookkeeping remain on the UI thread.

Cache validity is independent of History. Scoped mutations cover local transfer
batches, Trash/restore, folder creation and inbound sharing, including error and
partial-success paths. Observed directory ancestors and changed file subtrees get
revision changes; unrelated caches remain valid. Readers created during a mutation
also see it as active. A directory read that overlaps a write retries after a
100 ms delay; image results crossing a mutation are not cached. Registrations use
weak references so expired paths can be reclaimed, and existing cache capacity
limits remain in effect. This tracks in-process operations; external changes are
still checked by directory revalidation and thumbnail metadata checks.

Only source/API and structural checks were performed for these changes. No build,
unit tests, GUI execution or speedup measurements were run.
