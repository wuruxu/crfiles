# crFiles sync-v1

This is the first desktop, two-device synchronization implementation. It is
separate from a one-shot Share to transfer and from Android Quick Share/Connect.
Both crFiles applications must be running with nearby sharing enabled. Closing
a pane, hiding the window, or minimizing it does not remove a relationship.
Quitting the application stops synchronization; the next session reconciles disk
contents again. There is no system background daemon.

## User flow

Select one regular file or directory, then choose **Sync to**, immediately below
**Share to**. Only desktop peers advertising the sync extension through the
identity probe are listed. Select a device avatar to open **Sync to <device>**.
Leave **Update from remote** unchecked for A-to-B updates only, or check it
for two-way synchronization, then confirm with **Sync**. Compare pairing codes if needed and approve continuing access on the receiver.
Normal trusted-send acceptance does not substitute for this initial sync consent.

The receiver creates a fresh `Sync-<relationship-id>` directory under its configured
receive directory. For a selected directory, its contents become the contents of
that root. For a single file, the root contains that file under its original name.
The destination is shown in the receive consent prompt. Existing destination
folders are not merged during registration.

The titlebar menu opens **Syncing**, directly below **Sharing folders**. This
separate dialog follows the Trusted devices layout: the remote avatar, registration
and last successful reconciliation times, and the selected file/folder name followed
by **Read** and **Write** checkboxes. Both devices display A's grants to B:
Read allows A-to-B updates and Write allows B-to-A updates. Only A can edit them;
on B both checkboxes are disabled and identify A as their manager in tooltips.
Either device can pause or remove the relationship. The avatar opens the local
root; trailing icons pause/resume, open recovery
files and remove the relationship. Device and status details are available in
tooltips; errors also appear inline. Management remains available after a selected
file is gone.
Removing a relationship on either device removes that device's Syncing entry and
queues an authenticated removal notification to the other device. After receipt,
the other entry disappears as well. Files and recovery data remain on both disks.
Offline notifications survive restarts and retry when the pinned device is available.
The removed ID remains a revocation record and cannot be silently reactivated.
An authenticated data-sync retry against a durably removed ID receives `sync-revoked`
in place of the normal pairing acknowledgement; the caller also removes its local
entry and retains a tombstone instead of merely pausing.
Declining initial consent pauses the originating request instead of repeatedly
prompting. Canceling the initial transfer pauses that local relationship.

## Compatibility and identity

The existing mDNS service, transfer ALPN, TLS 1.3 channel, pairing-code exporter,
identity keys and legacy hello version remain unchanged. Discovery adds
`sync-v1` to TXT `caps`. The identity frame and authenticated hello add the boolean
`sync_v1` and integer `sync_permissions: 2`. Both are required in the authenticated
hello as well as the identity probe. Peers implementing only the original sync-v1
or the previous per-device permissions extension are excluded and rejected. They
must not interpret creator-controlled permissions as independent local grants. Normal Share
compatibility is unchanged. The independent `sync_removal: 1` capability in the
identity frame and authenticated hello enables proactive removal notifications.
A pending notification waits for that capability; normal data sync remains
compatible with peers supporting creator-controlled permissions only. Both peers
must update for automatic remote entry removal. Discovery and identity-probe
records are routing/capability hints only.

A sync initiator adds `sync: "<32 uppercase hexadecimal characters>"` to hello.
The transfer certificate must match the peer fingerprint selected at registration
and recorded in the relationship. A known paused, removed or mismatched
relationship rejects data sessions. A separate removal-only session may revoke
an existing paused/removed relationship after matching its stored fingerprint. Existing sync consent permits subsequent connections
for that relationship without another pairing prompt. It does not grant general
Share access or authorize a different directory. Forgetting a trusted device
also queues revocation of its sync relationships and cancels the active session.

Without `sync`, the existing `offer / accept / digest / stored / complete` flow
is unchanged. A peer without the permissions extension cannot receive a sync request. Sync never
falls back to a normal Share transfer, and no local absolute path is sent.

The device which created the relationship remains role A (0); the receiver is
role B (1). A schedules the connection; permitted changes travel over it in either direction.
B does not race A by simultaneously opening another connection for the same ID.
Manual Share sends queued before scheduling a sync session take priority.

## State and change detection

`nearby/sync/sync.sqlite3`, under the existing private crFiles identity location,
stores schema version 4, relationship settings, per-path versions and a pending
filesystem journal. SQLite uses WAL and FULL synchronous commits. A separate
filesystem lock excludes another process from using this sync store concurrently.
UI commands are queued; database and protocol operations are serialized across
transport generations. Watchers are constructed and destroyed on their own
sequenced task runner.

Every entry contains:

- `path`: normalized NFC UTF-8 relative path; directories have no trailing slash.
- `kind`: 0 for a deletion tombstone, 1 for a regular file, 2 for a directory.
- `size`: decimal byte length encoded as a string; zero except for regular files.
- `hash`: uppercase SHA-256 for regular files; empty for directories/tombstones.
- `a`, `b`: decimal revision counters encoded as strings, each below 2^62.

Each device increments its durable local relationship counter when a scan finds a
content/type/deletion change. It copies the previous entry's vector and replaces
its own component with that counter. File mtimes are used only to detect a file
changing while being read, not to choose a winning version. The SHA-256 describes
the bytes actually staged and transferred.

Recursive filesystem notifications invalidate snapshots. A complete scan is also
required on first use and at least every 30 seconds when connected. A normally
reconnects after 10 seconds; failures back off, with a maximum of 320 seconds for
failed reconciliation. Retries add up to 1.5 seconds of jitter to avoid repeated
collisions with another outgoing relationship. Discovery must supply a current endpoint and certificate
hint. Offline changes are found during the next full scan.

The root's filesystem identity is pinned and persisted. A missing/replaced root,
an incomplete scan, unreadable subtree, link, special file or nested filesystem
fails the scan instead of turning inaccessible contents into deletions. Root
removal is not propagated as deletion of the entire relationship.

## Access permissions and persisted display information

A is the creator and the authority for this relationship's permissions. The Read
bit permits A-to-B updates; the Write bit permits B-to-A updates. Both endpoints
store and display the same bits. A sends them over the authenticated connection;
B saves them and echoes them unchanged, and A rejects a different acknowledgement.
The worker also rejects local permission-edit commands on B, independently of the
disabled UI controls. B retains local pause/removal controls.

The initial **Update from remote** checkbox is unchecked: Read=true, Write=false.
Checking it creates Read=true, Write=true. Later A can edit both bits in Syncing.
Changing permissions interrupts A's active session, saves the policy and exchanges
it on the next connection; it does not unpause a relation. B shows its last received
policy while offline or paused, then adopts A's current policy on reconnection.
A failed durable local permission update pauses the relation and retries storage.
B must durably save the mirrored policy before acknowledging or transferring data.

For A, send=Read and receive=Write. For B, send=Write and receive=Read. If neither
direction is permitted, peers still exchange policy, then `sync-idle` without
scanning, transferring file contents or updating the last successful sync time.
These permissions control synchronization, not operating-system file permissions.

With only one permitted direction, the source is authoritative for its indexed
paths and is never modified by the receiver. The source sends its index first.
The receiver returns only matching-content receipts for those paths, using an
empty entry for nonmatches. It does not disclose private receiver-only names,
modified-file hashes or versions. Only the source donates bytes/versions. Changed
receiver copies can be replaced by the source and are retained in the receiver's
recovery archive. Receiver-only files are left alone. Unrelated descendants that
prevent replacing/deleting an ancestor cause a safe retry rather than recursive
deletion. Two permitted directions use the original vector merge/conflict rules.

A previously committed receive journal is not replayed after that incoming
direction is disabled: Write=false blocks replay on A; Read=false blocks replay
on B. The staged payload and originals remain available and the UI reports
recovery required. Re-enable the corresponding direction to permit replay or
follow the recovery steps. In particular, Write=false on B does not block its
normal receipt or crash recovery of A-to-B updates.
An individual filesystem operation already in progress can finish when a permission
change is requested, but the canceled session cannot start a new reconciliation.

The relationship JSON also persists the selected name, remote avatar, registration
time and last acknowledged successful reconciliation time. Display uses local
calendar/time formatting. A's legacy records preserve A's existing Read/Write
settings. B's legacy per-device grants are not a copy of A's policy: B displays
an unknown-permissions state until it receives A's authenticated settings. The
JSON `permissions: 2` marker distinguishes a saved creator-controlled policy from
legacy values. The role is retained and determines which endpoint owns the policy.
Missing timestamps remain unknown until a successful sync; missing display names
fall back to the stored single-file name or root basename. Both peers must upgrade.
SQLite user_version is raised to 4 with existing tables retained. Versions 1–3
upgrade on open; older apps reject version 4, preventing a downgrade from losing
pending removal notifications or reverting to editable per-device grants.

## Relationship removal

Local removal first saves `removed=true`, `paused=true`, `removal_pending=true`
in the relationship JSON. It stops watchers and removes the normal list entry.
The scheduler handles pending removals from either role, independently of pause
and data-sync permissions, using a current identity probe and the original
certificate fingerprint. It never falls back to sending files. Connection failures
retry with bounded exponential backoff and jitter; restarts preserve the pending
flag. Old tombstones without that field are treated as pending notifications so
previously stranded peer entries can also be cleared after upgrading.

The connection adds `sync_remove: true` alongside the relationship ID in hello.
Both authenticated hellos must advertise `sync_removal: 1`. This selects a dedicated
worker branch before any ordinary sync messages, consent, root access, hashing,
journal replay or file transfer. Known relationships must match the saved identity,
including after device trust was forgotten. The removal exception never adds a
trusted device or authorizes ordinary Share/data-sync access. The handshake does
not prompt for pairing again and uses a short acknowledgement timeout.

1. The removing device durably saves its tombstone before sending
   `sync-remove {id}`. Either A or B may initiate this control session.
2. The peer matches the frame ID to the authenticated hello, sets the matching
   relationship to removed/paused, stops its watcher, clears its notification flag
   and saves the tombstone. Only then does it send `sync-removed {id}`.
3. The initiator validates the acknowledgement ID and saves
   `removal_pending=false`. If that save fails, it retains the pending flag and
   retries. Repeated notifications and simultaneous removals are idempotent.

An unknown ID is acknowledged as a no-op without creating database entries or
filesystem objects. A known ID with a different fingerprint is rejected. Removing
an old ID cannot affect a newly created relationship for the same filename.
A failed tombstone write stops local synchronization, exposes the storage error,
queues a durable-write retry, and withholds acknowledgement. `sync-revoked` replies
on ordinary data sessions also require a durable tombstone before being sent.
No removal path deletes user files, recovery archives or the retained revocation ID.
Disabling nearby sharing or quitting the app postpones delivery until it is running
and sharing is enabled again; no system background daemon is introduced.

## Session messages

All control messages use the existing bounded, length-prefixed JSON framing.
File bytes are unframed, with lengths fixed by the agreed index. No control frame
is inserted into a raw byte stream.

1. A sends `sync-open` with `id`, `create`, `name`, `file`, `permissions: 2`,
   `read` and `write`. `sync-accepted` echoes that version and the exact same
   `read`/`write` bits. Missing, malformed or changed grants are rejected. `create` is true
   until A durably records acceptance. B either looks up the existing authorized
   relationship or obtains initial consent for a fresh root. B saves registration
   before responding `sync-accepted`. Initial rejection returns `sync-declined`.
   A sync pairing rejection can also return `sync-declined` instead of `paired`.
   Rejection keeps the channel open only to send the negative reply; it never
   permits file I/O. A pauses the relationship durably and the Sync to dialog
   re-enables device selection. Retrying the same device reuses the ID and original
   source, applies the selected permission and requests consent again. Removed
   relationships cannot be resumed. Failed pause writes remain paused in memory
   and retry persistence; transient transport failures retain automatic retry.
2. Each side opens/pins its root, replays its journal and obtains a current index.
   For two-way access A sends its index, then B sends its index; for one-way
   access the source sends first and the receiver returns receipts. `sync-index` contains `entries`
   (at most 32) and a required `last` boolean. Empty non-final pages are invalid.
3. Both derive the same plan in role-A/role-B order. They exchange `sync-plan`
   containing the SHA-256 of canonical JSON plan rows with newline separators.
   Rows include final metadata, destination path, donor role and source path.
4. For each file whose contents are needed on either side, the donor makes a
   private verified staging copy. It sends `sync-content` with the agreed `path`,
   `hash` and `sending` flag. When the recipient needs bytes, exactly the planned
   size follows. The recipient flushes and verifies its staging file, then sends
   `sync-staged`. A local conflict copy can use the same donor data without a
   redundant network transfer.
5. All payloads are staged before either side changes originals. Each side
   validates local preconditions, writes recovery metadata and commits its entire
   pending plan in one database transaction. It then applies individual journal
   records, committing each entry's new vector and removing its pending record
   together. Obsolete children are removed before parents; live parents are
   created before descendants.
6. After local application, each side sends `sync-committed` and waits for its
   peer's acknowledgement. A lost acknowledgement is reconciled from durable
   versions on the next connection; it does not blindly replay old writes.

`sync-heartbeat` frames may appear while a peer scans, hashes, stages or applies
files and the other peer is expecting a control frame. These keep long filesystem
operations from expiring the control-frame timeout. They are consumed by the
sync adapter and never appear inside file payloads.

## Merge and conflict rules

A version dominates another when both components are at least as large and at
least one is larger. A dominating version wins. Identical contents merge the
component-wise maximum vector without retransmitting the file.

Concurrent different regular-file versions keep A's content at the original
path and preserve B's content as a normal synchronized conflict file. Its name is
`sync-conflict-<path-hash-prefix>-<version-and-content-hash-prefix>` in the same
parent. If that name already contains different data, an additional deterministic
suffix is used instead of overwriting it. Both contents reach both devices.

A concurrent edit wins over a deletion. Concurrent file/directory changes retain
the directory and preserve the file as a conflict copy. A surviving descendant
revives a deleted/replaced ancestor directory, retaining any displaced regular
file. This prevents deleting a newly edited child merely because its parent was
deleted on the other device.

A fresh local edit detected before plan commit aborts that attempt and triggers a
rescan. If an original changes after journaling but before any archive move, the
incoming file can be kept as a conflict copy while the original is rescanned.
If recovery cannot establish an unambiguous precondition after a crash or external
filesystem modification, it preserves the originals, archive and staged payload
and reports that recovery needs review. It does not guess and overwrite data.

## Recovery files

Every root reserves `.crfiles-sync/<relationship-id>/`. Its contents are excluded
from scans and cannot be named by a remote index. The root-level `.crphotos-lock`
is also excluded for compatibility with the Windows receiver lock.

An operation uses a random local 128-bit ID:

- `<operation>.json` records the original relative path and before/after metadata.
- `<operation>.saved` retains the file or empty directory moved out of that path.
- `<operation>.part` is a verified or incomplete staging file; it must not be
  treated as a completed receive without checking the journal and hash.

Replacements and propagated deletions retain the old data. There is no automatic
retention purge in v1. Routine unneeded staging files are discarded when a session
unwinds; an abrupt process exit can leave unreferenced staging files. Pending
staging files are retained for crash recovery. Recovery data consumes additional
space and is local to each device.

Use Recovery files to inspect the metadata and saved files. Pause the relationship
before manually restoring a saved file to the `path` recorded in its JSON file.
A restoration becomes a new local change after resuming. For a persistently
ambiguous journal, keep the recovery directory, remove the relationship, recover
the desired contents, then establish a new relationship. Do not delete the sync
database or recovery directory to clear an error while a relationship is active.

## First-version boundaries

- Linux, Windows and macOS desktop peers on the existing private IPv4 transport.
- One selected file or directory per relationship; no multi-device mesh.
- At most 128 active relationships and 4096 retained registrations/revocations.
- At most 100000 indexed paths including tombstones; 32 MiB of aggregate index
  path bytes; 4096 bytes per path; 64 components; 240 UTF-8 bytes per component.
- Regular files up to 16 GiB. Changed files are transferred whole. Payloads for
  a reconciliation are staged before application, so sufficient temporary space
  is required on both devices. No block-level delta or byte-offset resume.
- Directory renames are reconciled as deletes/additions. A selected single file
  is scoped to its original name and its conflict copies; renaming that selected
  file requires a new relationship. Renaming/replacing a selected directory root
  likewise requires a new registration.
- Overlapping roots are rejected, including selected single files sharing one
  parent. A synchronized directory must not contain the sync database. A single
  file such as `~/001.txt` may use an ancestor of the database as its pinning root:
  only the selected file and its conflict copies are indexed. Selecting files
  inside the database directory itself remains forbidden.
- Links, special files, nested mounted filesystems, permission bits, extended
  attributes, alternate data streams and file ownership are not synchronized.
- NFC names are used on the wire. Existing decomposed spellings are resolved
  locally. Canonical/case-folding collisions and cross-platform reserved names
  fail safely. Case-only renames involving retained tombstones require resolving
  the collision or starting a new relationship.
- This is eventual per-file synchronization, not a filesystem-wide atomic snapshot
  or a document merge system. Concurrent editors can produce conflict copies.

## Manual acceptance after an authorized build

No build or device interoperability run was performed during this implementation.
Use `autoninja` for any subsequently authorized build. No unit-test source was added.

1. Verify legacy file/tree Share to and Android Quick Share still work. Check
   that old peers do not appear in Sync to and cannot receive sync traffic.
2. Pair two devices, reject initial sync consent, and check that the sender pauses.
   Resume and accept; verify the exact destination and that unrelated files remain.
3. Verify both dialogs, device avatars, the unchecked Update from remote checkbox,
   the **Sync** button, localized dates and the titlebar menu ordering. Sync a file,
   nested folders, empty files and empty directories. With the checkbox off,
   verify updates flow A-to-B only and both lists show Read checked, Write unchecked.
   B's checkboxes must be disabled. With it on, verify two-way convergence and both
   lists checked. Change Read/Write on A; check B mirrors the policy after connecting,
   including all four combinations, cancellation during transfer and restart.
   B must reject permission-edit commands but still support pause/removal. Check
   pending-journal replay against each role's incoming direction, legacy B's unknown
   state before receiving A's policy, and rejection of old peers.
4. Close panes and hide/minimize windows. Edit using another application and verify
   synchronization continues. Quit, edit offline, restart, and verify reconciliation.
5. Disconnect both devices and edit the same file differently; reconnect and verify
   both versions on both devices. Repeat with delete/edit, directory delete/new
   child, file/directory replacement and a previously edited conflict filename.
6. Delete files and directories on each side. Verify propagation, tombstones and
   recoverable saved files. Restore a saved file while paused and verify its return.
7. Disconnect during staging and after a peer commits. Terminate a process after
   journal commit and during application. Verify restart replay or a clear recovery
   error with preserved data, never an unchecked overwrite.
8. Edit an original while a large incoming file is staging. Check that the local
   edit is retained or creates a conflict, including editor atomic-save patterns.
9. Remove on A with B online: both list entries must disappear and both files and
   recovery archives must remain. Repeat from B, with the peer paused, with a missing
   root, and with both devices removing simultaneously. Remove while the peer is
   offline, restart both apps, reconnect and verify delivery. Lose the acknowledgement
   and repeat the notification; verify idempotence. Create a new relationship for the
   same filename before the old removal arrives; the new ID must remain untouched.
   Exercise mismatched fingerprints/IDs, unknown IDs, old peers lacking sync_removal,
   forgotten device trust and disk errors before each durable save. Failed saves
   must not acknowledge removal or restart data sync; pending notifications must
   resume after recovery. Verify normal Share and ongoing nonremoved sync still work.
10. Exercise missing drives, replaced roots, permission failures, full disks,
    links/junctions, canonical/case collisions, Unicode names and overlapping roots.
    Register `~/001.txt` while the database is under the same home directory and
    verify the receiver is prompted. Selecting the whole home directory or a file
    inside the sync database directory must still be rejected. Check that a failed
    registration updates the originating Sync to dialog rather than waiting forever.
    A scan failure must not propagate mass deletions. Verify macOS sandbox access
    prompts and persistent access after restart, and Windows locked-file behavior.

11. Keep the Sync to dialog open, reject the request on B, and verify A pauses
    without automatic consent prompts. Click the avatar again, change Update from
    remote, and press Sync. Verify the same ID is reused, B is prompted again, and
    approval starts copying with the newly selected permission. Repeat rejection
    at pairing and folder consent, with trusted/untrusted peers, and with a fast
    response before the next UI refresh. Verify normal Share rejection still cancels
    immediately and removing a paused relation cannot be undone by a queued retry.
