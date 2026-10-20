// Shared-memory protocol between P2Render.exe (x64 app) and p2r_core.dll (x86, injected into
// portal2.exe). The core creates Local\p2render_ctrl_<game pid> once the engine is up; the app
// writes a command, then increments cmd_seq; the core copies cmd_seq to ack_seq when it took it.
// Only fixed-size types: same layout in both.
#pragma once
#include <stdint.h>

enum { P2R_CTRL_MAGIC = 0x4C525443, P2R_CTRL_VERSION = 1 };

enum P2RCoreState : int32_t {
    CORE_STARTING = 0,   // injected, engine not up yet
    CORE_READY = 1,      // engine up, D3D9Ex device, waiting for a job
    CORE_BUSY = 2,       // a job is running
    CORE_BROKEN = 3,     // cannot render (message says why)
};

enum P2RCommand : int32_t { CMD_NONE = 0, CMD_RENDER = 1, CMD_CANCEL = 2, CMD_QUIT = 3 };

enum P2RJobState : int32_t {
    JOB_NONE = 0,
    JOB_LOADING = 1,     // demo loading
    JOB_RECORDING = 2,   // frames are being captured
    JOB_ENCODING = 3,    // capture over, encoder finishing / muxing
    JOB_DONE = 4,
    JOB_FAILED = 5,
};

struct P2RJob {
    char demo[260];          // playdemo argument (relative to the game folder: portal2\)
    wchar_t out_path[520];   // final .mp4
    int32_t fps;
    int32_t codec;           // P2RCodec (shared.h)
    int32_t bitrate_kbps;    // 0 = from the resolution
    int32_t show_window;     // 0 = do not present while rendering (faster)
};

struct P2RControl {
    uint32_t magic, version;
    volatile int32_t core_state;   // P2RCoreState
    char core_message[256];

    // App -> core.
    volatile int32_t cmd;          // P2RCommand
    volatile int32_t cmd_seq;
    P2RJob job;

    // Core -> app.
    volatile int32_t ack_seq;
    volatile int32_t job_id;       // = cmd_seq of the CMD_RENDER being reported
    volatile int32_t job_state;    // P2RJobState
    volatile int32_t frames;       // frames captured
    volatile int32_t frames_encoded;
    volatile int32_t render_ms;    // capture duration so far
    int32_t width, height;
    char job_message[256];
};
