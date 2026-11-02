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
identity probe are listed. Select a device, compare pairing codes if needed, and
approve continuing two-way access on the receiver. Normal trusted-send acceptance
does not substitute for this initial sync consent.

The receiver creates a fresh `Sync-<relationship-id>` directory under its configured
receive directory. For a selected directory, its contents become the contents of
that root. For a single file, the root contains that file under its original name.
The destination is shown in the receive consent prompt. Existing destination
folders are not merged during registration.

The same dialog lists relationships, their paths and status. It supports Pause,
Resume, Remove sync, Open and Recovery files. The empty-area context menu also
opens this dialog, so management remains available after a selected file is gone.
Removing a relationship leaves files and recovery data on disk. The removed ID
is retained as a revocation record; it cannot be silently reactivated by a peer.
An authenticated retry against that ID receives `sync-revoked` in place of the
normal pairing acknowledgement, which pauses the originating relationship.
Declining initial consent pauses the originating request instead of repeatedly
prompting. Canceling the initial transfer pauses that local relationship.

## Compatibility and identity

The existing mDNS service, transfer ALPN, TLS 1.3 channel, pairing-code exporter,
identity keys and legacy hello version remain unchanged. Discovery adds
`sync-v1` to TXT `caps`. The identity frame and authenticated hello add the boolean
`sync_v1`. Discovery and identity-probe records are routing/capability hints only.

A sync initiator adds `sync: "<32 uppercase hexadecimal characters>"` to hello.
The transfer certificate must match the peer fingerprint selected at registration
and recorded in the relationship. A known paused, removed or mismatched
relationship is rejected. Existing sync consent permits subsequent connections
for that relationship without another pairing prompt. It does not grant general
Share access or authorize a different directory. Forgetting a trusted device
also queues revocation of its sync relationships and cancels the active session.

Without `sync`, the existing `offer / accept / digest / stored / complete` flow
is unchanged. A peer without `sync_v1` cannot receive a sync request. Sync never
falls back to a normal Share transfer, and no local absolute path is sent.

The device which created the relationship remains role A (0); the receiver is
role B (1). A schedules the connection, but both peers send changes over it.
B does not race A by simultaneously opening another connection for the same ID.
Manual Share sends queued before scheduling a sync session take priority.

## State and change detection

`nearby/sync/sync.sqlite3`, under the existing private crFiles identity location,
stores schema version 1, relationship settings, per-path versions and a pending
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

## Session messages

All control messages use the existing bounded, length-prefixed JSON framing.
File bytes are unframed, with lengths fixed by the agreed index. No control frame
is inserted into a raw byte stream.

1. A sends `sync-open` with `id`, `create`, `name`, and `file`. `create` is true
   until A durably records acceptance. B either looks up the existing authorized
   relationship or obtains initial consent for a fresh root. B saves registration
   before responding `sync-accepted`. Initial rejection returns `sync-declined`.
2. Each side opens/pins its root, replays its journal and obtains a current index.
   A sends its index, then B sends its index. `sync-index` contains `entries`
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
  parent. Sync state cannot be placed inside a synchronized root.
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
3. Sync a single file, nested folders, empty files and empty directories. Compare
   hashes, modify on each device, and verify convergence without repeated sends.
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
9. Pause/remove from either side, revoke device trust, disable nearby sharing, and
   restart. Verify stopped/revoked relationships cannot silently resume. Check that
   disk errors during saving controls are visible and stop local synchronization.
10. Exercise missing drives, replaced roots, permission failures, full disks,
    links/junctions, canonical/case collisions, Unicode names and overlapping roots.
    A scan failure must not propagate mass deletions. Verify macOS sandbox access
    prompts and persistent access after restart, and Windows locked-file behavior.
