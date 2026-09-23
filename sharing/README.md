# crPhotos desktop transport compatibility

The ten C++ transport files in this directory were imported from the local
`crPhotos/sharing` implementation on 2026-09-22. The source repository HEAD was
`894eeda09e0940d1b2123942ee63508b9374236f`. They remain separate from UI code so that
protocol fixes can be ported without changing the file-browser views.

Original import adaptations:

- Application includes, include guards, resource identifiers and C++ namespaces
  are named crFiles.
- Settings are provided by `crfiles_theme.h` with a crFiles device name. Automatic
  acceptance of trusted sends remains disabled. This UI does not persist a trust
  approval, so unknown peers require code comparison for every new session.
- Identity and Connect credential directories use crFiles. Default received files
  go to Downloads/crFiles Received rather than Pictures/crPhotos Received.

Wire-level values are deliberately unchanged: `_crphotos._tcp.local`, service
version 1, the desktop and identity ALPNs and TLS exporter label. The original
framed transfer remains available alongside the optional extensions below. The per-directory `.crphotos-lock` name also
remains unchanged to coordinate two applications receiving into the same folder.
The four-record PTR/SRV/TXT/A advertisement, expiry/goodbye behavior and private-IPv4
restrictions are inherited unchanged. Do not rename wire identifiers when
renaming application classes. Android Connect's HTTPS service is separate from
the desktop framed TLS protocol and is not exposed by the file-browser UI.

The original import was source-compared before the extensions below. No network
interoperability run has been performed for the extended crFiles implementation.

## Manual verification after an authorized build

1. Run crFiles and crPhotos on two devices on the same private IPv4 network. Enable
   nearby sharing on each and verify the configured names appear in both lists.
2. Send a selected regular file in each direction. Compare the full pairing code
   on both devices; verify rejecting pairing never starts a file transfer.
3. Accept pairing, then reject the receiver's file offer. Verify no completed file
   appears. Repeat with receive approval and compare received file hashes.
4. Cancel an active send, close/reopen the panel, and start another session. Verify
   an old prompt cannot approve or cancel the newer session.
5. Disable sharing and confirm the peer disappears. Re-enable and confirm discovery
   returns. Check independent identities by running both apps on the same host.
6. Check inaccessible receive paths, empty files, duplicate names, and the existing
   crPhotos protocol limits. Folders and symlinks are not regular-file transfers.

For the upstream packet and security details, consult
`../../crPhotos/sharing/PROTOCOL.md` and `../../crPhotos/sharing/MDNS_INTEROP.md`.

## Optional crFiles targets and directory trees

Discovery adds `caps=targets,tree` to TXT; service name and version remain unchanged.
The small identity frame adds `folder_transfer: true`. An extension-capable probe
then reads a second `targets` frame containing `{id, name}` entries; filesystem
paths are never advertised. Legacy probes can read the first frame and close.
Catalog reads are limited to 32 KiB and 128 targets. Catalog results are hints;
TLS pairing and receive consent remain mandatory unless existing local trust
preferences explicitly authorize auto-acceptance.

The paired `hello` also advertises `folder_transfer` and the current target list.
An offer may include `target`, a stable receiver-issued UUID. Missing, withdrawn
or changed target registrations are rejected before writing; withdrawal cancels
an in-flight receive. Without a target, the legacy default receive directory is
used. Each batch is stored below the chosen root in a private Received-* folder.

A `tree: true` offer permits relative names and general file types. Directory
entries end in `/` with size `0`; they exchange the SHA-256 of empty input and a
normal `stored` acknowledgement. Explicit entries preserve empty directories.
Every component is validated, duplicate paths are rejected case-insensitively,
and traversal, absolute paths, device names, links and special files are rejected.
Linux walks destination parents with no-follow directory descriptors; Windows
pins each ancestor against substitution. Size/hash/cancellation checks are the
same as the legacy stream. A failed transfer may leave its isolated partial batch.

Tree manifests are bounded to 4096 entries, 64 path components, 384 KiB of JSON,
16 GiB per file and 64 GiB per batch. Plain legacy media offers retain their
original format and 128-file limit. Directories and general non-media files require
an extension-capable receiver; they are never flattened or silently sent as a
legacy media transfer. Existing crPhotos clients keep their original default
receive target and supported media types; selecting named panes and directory
transfer require a client implementing these extensions.
