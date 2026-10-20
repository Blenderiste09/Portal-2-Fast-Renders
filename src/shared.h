// Shared-memory protocol between the game (d3d9.dll proxy, x86) and p2r_encoder.exe (x64).
// Same layout in both: only fixed-size types, no pointers.
//
// The game copies every movie frame into one of P2R_SLOTS shared D3D9Ex textures (frame n uses
// slot n % P2R_SLOTS) and releases the "ready" semaphore once the GPU copy is complete. The
// encoder copies the slot into its own texture, releases the "free" semaphore and encodes.
// Kernel object names: Local\p2r_<id>_{ready,free,started,done}, mapping Local\p2r_<id>_mem.
#pragma once
#include <stdint.h>

enum { P2R_MAGIC = 0x52325050, P2R_VERSION = 2, P2R_SLOTS = 8 };

enum P2RCodec : uint32_t { P2R_H264 = 0, P2R_HEVC = 1 };

enum P2REncoderState : int32_t {
    P2R_ENC_STARTING = 0,
    P2R_ENC_RUNNING = 1,   // shared textures opened, ready for frames
    P2R_ENC_MUXING = 2,
    P2R_ENC_DONE = 3,
    P2R_ENC_FAILED = 4,
};

struct P2RShared {
    uint32_t magic, version;
    uint32_t width, height, fps;
    uint32_t codec;              // P2RCodec
    uint32_t bitrate_kbps;
    uint32_t num_slots;
    uint64_t slot_handles[P2R_SLOTS];  // D3D9Ex shared handles (global, valid in any process)
    wchar_t out_path[520];       // final .mp4
    wchar_t wav_path[520];       // engine audio (startmovie ... wav), muxed at the end if present

    // Advanced mode: frames are read back (D3D11 staging) and piped to ffmpeg as raw BGRA.
    int32_t use_ffmpeg;
    wchar_t ffmpeg_path[520];
    wchar_t ff_video_args[1024]; // output options of the video pass, e.g. "-c:v libx264 -crf 18"
    wchar_t ff_audio_args[256];  // audio options of the mux pass, e.g. "-c:a aac -b:a 320k"

    // Game -> encoder.
    volatile int32_t ended;            // no more frames after total_frames
    volatile int32_t total_frames;     // valid once ended != 0

    // Encoder -> game.
    volatile int32_t enc_state;        // P2REncoderState
    volatile int32_t frames_encoded;
    char error[256];
};
