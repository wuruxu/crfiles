# File icon assets

The generic MIME icons come from Adwaita. The project document, source code,
storage device, camera, lock and trash icons come from GNOME-Colors. The
GNOME-Colors files are bundled from the `mimetypes`, `devices`, `status` and
`places` directories under `/usr/share/icons/gnome-colors-common/scalable`.

Local folders, archive entries and SMB shares use file names and MIME types to
select bundled file icons. Project documentation (including AUTHORS, CREDITS
and INSTALL), Makefiles, patches, Python, CSS, C-family, Java, PHP and Ruby
source files, and BitTorrent files have dedicated icons. The
default file icon is Adwaita's lined document, `text-x-generic.svg`. Windows
prefers registered Shell icons for local paths and uses the bundled set as a
fallback. Device icons apply to recognized local mount points or drive roots.

crFiles loads the bundled SVGs on demand, rasterizes them at the requested
size and shares the cached images across panes. See COPYRIGHT for the upstream
attribution and license texts, including the GNOME-Colors GPL version 2 notice.
