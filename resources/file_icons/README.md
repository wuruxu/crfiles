# File icon assets

Unmodified SVG files imported from `/usr/share/icons/Adwaita/scalable/mimetypes`,
by the GNOME Project (https://www.gnome.org).

Files without a dedicated MIME icon use Adwaita's `text-x-generic.svg`
(the lined document) instead of the blank `application-x-generic.svg`.
This is crFiles' fallback choice; the upstream SVG is unchanged. Windows
continues to prefer registered system icons and uses this fallback for unknown
file types or when the system icon is unavailable.

These project assets are used on every platform. Runtime code maps MIME types
to this fixed asset set; it never scans `/usr/share/icons` or a theme directory.
SVGs are loaded and rasterized on demand for the current icon size. Cached images
are shared across panes. See COPYRIGHT for attribution and the upstream license
terms, including the CC-BY-SA 3.0 license text. No PNG or other raster source
assets are used for file type icons.
