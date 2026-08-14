# FiFi shared API

This directory owns contracts consumed across FiFi platform implementations
and applications. `ipc.h` is wire API version 1: messages use an eight-byte
header containing little-endian 32-bit type and payload-length fields.

Compatible additions may define a new message ID without changing the version.
Changing framing, an existing ID, or an existing payload layout requires a new
API version and a compatibility path before either platform adopts it.
