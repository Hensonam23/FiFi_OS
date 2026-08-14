#ifndef FIFI_SHARED_IPC_H
#define FIFI_SHARED_IPC_H

/* Stable wire contract shared by FiFi compositors and desktop applications.
 * Increment FIFI_IPC_VERSION only for an incompatible payload or framing
 * change. New optional message types do not require a version bump. */
#define FIFI_IPC_VERSION 1u
#define IPC_HDR_SZ       8

/* Application to compositor. */
#define IPC_APP_CONNECT   0x01u /* uint16_t width, height; char title[60] */
#define IPC_APP_FRAME     0x02u /* uint32_t x, y, width, height, pixels[] */
#define IPC_APP_TITLE     0x03u /* UTF-8 title bytes */
#define IPC_APP_CLOSE     0x04u /* no payload */

/* Compositor to application unless noted otherwise. */
#define IPC_WIN_CREATED   0x10u /* uint32_t id, x, y, width, height */
#define IPC_INPUT_KEY     0x11u /* uint8_t key */
#define IPC_INPUT_MOUSE   0x12u /* int32_t x, y; uint8_t buttons; int8_t scroll */
#define IPC_FOCUS         0x13u /* no payload */
#define IPC_INPUT_GAMEPAD 0x14u /* uint16_t buttons; six int16_t axes */
#define IPC_INVALIDATE    0x15u /* no payload */
#define IPC_NOTIFY        0x16u /* app to compositor: UTF-8 text */
#define IPC_CLIP_SET      0x17u /* app to compositor: UTF-8 text */
#define IPC_CLIP_GET      0x18u /* app to compositor: no payload */
#define IPC_CLIP_DATA     0x19u /* UTF-8 text */
#define IPC_OPEN_FILE     0x1Au /* app to compositor: path bytes */
#define IPC_WIN_RESIZE    0x1Bu /* uint16_t width, height */
#define IPC_DRAG_START    0x1Cu /* app to compositor: path bytes */
#define IPC_DROP_FILE     0x1Du /* path bytes */
#define IPC_SET_WALLPAPER 0x1Eu /* app to compositor: path bytes */
#define IPC_ADD_DESK_ICON 0x1Fu /* app to compositor: path NUL label */

#endif
