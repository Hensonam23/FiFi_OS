# FiFi shared API

This directory owns contracts shared by the FiFi Linux compositor and its
applications. `ipc.h` is wire API version 1: messages use an eight-byte
header containing little-endian 32-bit type and payload-length fields.

Compatible additions may define a new message ID without changing the version.
Changing framing, an existing ID, or an existing payload layout requires a new
API version and a compatibility path before Linux desktop consumers adopt it.

`theme.h` is theme API version 1. It fixes the wallpaper, image-fit, panel,
accent-palette, and font-size identifiers used by the compositor GUI and
Settings app. It also owns the persisted configuration's keys and defaults so
both consumers show the same state before the file exists. The key/value file
carries `theme_format=1`; readers remain compatible with older files that omit
it. Storage and file permissions remain Linux platform responsibilities.
