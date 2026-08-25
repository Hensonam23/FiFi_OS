# FiFi shared API

This directory owns contracts shared by the FiFi Linux compositor and its
applications. `ipc.h` is wire API version 1: messages use an eight-byte
header containing little-endian 32-bit type and payload-length fields.
The frozen public contract and change rules are documented in
[`docs/LINUX_DESKTOP_API.md`](../../docs/LINUX_DESKTOP_API.md).

Compatible additions may define a new message ID without changing the version.
Changing framing, an existing ID, or an existing payload layout requires a new
API version and a compatibility path before Linux desktop consumers adopt it.

`app_ipc.h` is the Linux native-application transport for that wire contract.
It owns reliable complete writes, compositor socket registration, and bounded
frame construction so applications do not each implement subtly different
framing and partial-write behavior.

`app_ui.h` is bitmap UI foundation API version 1. It owns validated PSF2 font
loading, clipped framebuffer primitives, glyph and text drawing, truncation,
and word wrapping. Native applications can share these mechanics while keeping
their own layouts and visual identities.

`theme.h` is theme API version 1. It fixes the wallpaper, image-fit, panel,
accent-palette, and font-size identifiers used by the compositor GUI and
Settings app. It also owns the persisted configuration's keys and defaults so
both consumers show the same state before the file exists. The key/value file
carries `theme_format=1`; readers remain compatible with older files that omit
it. Storage and file permissions remain Linux platform responsibilities.
