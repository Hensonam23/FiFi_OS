# FiFi Linux Desktop API

This document freezes the first public contract between the FiFi compositor,
native desktop applications, and Settings. The canonical definitions are
`fifi/shared/ipc.h`, `fifi/shared/app_ipc.h`, and `fifi/shared/theme.h`.

## Compatibility rules

- IPC and theme API version 1 are stable. Existing message numbers, payload
  layouts, identifiers, configuration keys, and their meanings must not change.
- Additive message types and optional configuration keys may be introduced
  without changing a version. Existing readers must safely ignore what they do
  not understand.
- Any incompatible framing, payload, identifier, or key-semantics change needs
  a new version and a compatibility path before a producer adopts it.
- Integers on the wire are little-endian. Strings are UTF-8 byte sequences and
  are not NUL-terminated unless a payload description explicitly says so.

## Native application IPC version 1

The compositor listens on `/tmp/fifi-compositor.sock`. The image restricts the
socket to the FiFi desktop account/group, and the compositor accepts only UID 0
or the desktop UID 1000. This is a local desktop protocol, not a network API.

Every message starts with two little-endian `uint32_t` values: message type and
payload length. Payloads larger than 64 MiB are rejected. Native apps should use
`app_ipc.h`, which handles complete writes, connection registration, bounded
full-frame messages, startup retries, and `SIGPIPE` suppression.

Application-to-compositor messages:

| Name | Payload |
|---|---|
| `IPC_APP_CONNECT` | `uint16_t width`, `uint16_t height`, up to 64 title bytes |
| `IPC_APP_FRAME` | `uint32_t x`, `y`, `width`, `height`, then 32-bit pixels |
| `IPC_APP_TITLE` | UTF-8 title bytes |
| `IPC_APP_CLOSE` | Empty |
| `IPC_NOTIFY` | UTF-8 notification text |
| `IPC_CLIP_SET` | UTF-8 clipboard text |
| `IPC_CLIP_GET` | Empty |
| `IPC_OPEN_FILE` | Path bytes |
| `IPC_DRAG_START` | Path bytes |
| `IPC_SET_WALLPAPER` | Path bytes |
| `IPC_ADD_DESK_ICON` | Path, NUL, label |

Compositor-to-application messages:

| Name | Payload |
|---|---|
| `IPC_WIN_CREATED` | Five `uint32_t`: id, x, y, width, height |
| `IPC_INPUT_KEY` | One FiFi key byte |
| `IPC_INPUT_MOUSE` | `int32_t x`, `int32_t y`, button byte, scroll byte |
| `IPC_FOCUS` | Empty |
| `IPC_INPUT_GAMEPAD` | `uint16_t buttons`, then six `int16_t` axes |
| `IPC_INVALIDATE` | Empty |
| `IPC_CLIP_DATA` | UTF-8 clipboard text |
| `IPC_WIN_RESIZE` | `uint16_t width`, `uint16_t height` |
| `IPC_DROP_FILE` | Path bytes |

## Theme and settings version 1

The persisted file is `/fifi-data/fifi-settings.conf`. New writers include
`theme_format=1`; readers remain compatible with older files that omit it.
Settings and the compositor use the key and default constants in `theme.h`.

Stable value ranges are:

- wallpaper IDs: `0..12`
- image-fit IDs: `0..3`
- panel edges: bottom, top, left, right (`0..3`)
- panel alignment: start, center, end (`0..2`)
- corner radius: `0..12`
- font size: `6..96` pixels; the shared picker exposes the supported presets
- UTC offset: `-12..14`

Boolean keys use `0` or `1`. Accent values are unsigned `0x00RRGGBB` values
written in decimal. Unknown keys are ignored when read. Storage ownership,
atomicity, and permissions remain Linux platform responsibilities.

## Change checklist

Before merging a contract change:

1. Update the canonical shared header and this document together.
2. Preserve version 1 behavior or introduce a new version and compatibility path.
3. Run `make linux-shared-api-test` and all tests for affected consumers.
4. Build the initramfs and pass `make linux-qemu-test`.
5. Re-test affected input, graphics, or hardware behavior on the laptop when the
   change crosses a hardware boundary.
