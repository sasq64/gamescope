#pragma once

// Wire protocol between gamescope's libretro backend (src/Backends/LibretroBackend.cpp)
// and the libretro core that spawned it (src/libretro/core.cpp).
//
// The two ends are joined by a single AF_UNIX SOCK_SEQPACKET socketpair, created by
// the core before it forks. The child inherits one end and is told its number with
// `--libretro-fd <n>`; there is no path in the filesystem and so nothing to clean up,
// and no window in which a stranger could connect.
//
// SEQPACKET preserves message boundaries, so every read is exactly one message and
// there is no framing to get wrong. Frames travel as dmabuf fds sent once, at the
// hello, and thereafter only a slot number moves per frame.
//
// Directions are strict: gamescope only ever sends HELLO/FRAME/BYE, the core only
// ever sends INPUT/RELEASE. Each direction has a single writer thread, so no message
// interleaves with another.

#include <stdint.h>

#define GSLR_PROTOCOL_VERSION 2u

// Buffers in the ring. Three lets gamescope compose the next frame while the core
// still holds the last one, without ever waiting on it.
#define GSLR_NUM_BUFFERS 3u

enum gslr_msg_type
{
    // gamescope -> core
    GSLR_MSG_HELLO   = 1, // struct gslr_hello, plus GSLR_NUM_BUFFERS fds via SCM_RIGHTS
    GSLR_MSG_FRAME   = 2, // struct gslr_frame
    GSLR_MSG_BYE     = 3, // no payload

    // core -> gamescope
    GSLR_MSG_INPUT   = 16, // struct gslr_input
    GSLR_MSG_RELEASE = 17, // struct gslr_release
};

struct gslr_header
{
    uint32_t type;   // gslr_msg_type
    uint32_t length; // payload bytes following this header
};

// Sent once, immediately after the backend has allocated its ring. The fds ride
// along in the same sendmsg as SCM_RIGHTS, in slot order.
struct gslr_hello
{
    uint32_t version;     // GSLR_PROTOCOL_VERSION
    uint32_t width;
    uint32_t height;
    uint32_t drm_format;  // DRM_FORMAT_XRGB8888
    uint32_t num_buffers;
    uint32_t refresh_mhz; // what gamescope will actually pace at
    uint64_t modifier;    // DRM format modifier, shared by every buffer
    uint32_t stride[GSLR_NUM_BUFFERS];
    uint32_t offset[GSLR_NUM_BUFFERS];
    uint64_t size[GSLR_NUM_BUFFERS]; // bytes to mmap
};

// One composited frame is readable in `slot`. The core must answer with a RELEASE
// for that slot once it has finished reading the pixels.
struct gslr_frame
{
    uint32_t slot;
    uint32_t _pad;
    uint64_t serial; // monotonic, so a dropped FRAME is visible as a gap

    // The centred part of the frame the focused window actually covers, in frame
    // pixels: a 640x480 client in a 1280x1024 session is scaled to 1280x960 and
    // the rest is border. 0 when there is nothing focused to measure.
    uint32_t used_width;
    uint32_t used_height;
};

struct gslr_release
{
    uint32_t slot;
    uint32_t _pad;
};

enum gslr_input_type
{
    GSLR_INPUT_KEY    = 1, // code = Linux evdev KEY_*, value = 1 down / 0 up
    GSLR_INPUT_BUTTON = 2, // code = Linux evdev BTN_*, value = 1 down / 0 up
    GSLR_INPUT_MOTION = 3, // x, y = relative delta in pixels
    GSLR_INPUT_WARP   = 4, // x, y = absolute position, normalized 0..1
    GSLR_INPUT_WHEEL  = 5, // x, y = scroll delta
};

struct gslr_input
{
    uint32_t type; // gslr_input_type
    uint32_t code;
    int32_t  value;
    float    x;
    float    y;
};
