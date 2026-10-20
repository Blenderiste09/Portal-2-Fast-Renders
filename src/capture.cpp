#include "capture.h"

#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d9ex.h"
#include "engine.h"
#include "log.h"
#include "shared.h"

namespace capture {

struct Options {
    char name[128];
    char demo[260];
    int fps = 60;
    P2RCodec codec = P2R_H264;
    int bitrateKbps = 0;   // 0 = from the resolution
    bool window = true;    // present to the window while rendering
};

enum class State { Idle, Arming, Recording, Finishing };

struct Session {
    State state = State::Idle;
    Options opt;
    int armFrames = 0;

    // Saved cvars, restored at the end.
    char queueMode[32], hostFramerate[32], fpsMax[32];

    // GPU ring (created on the first movie frame, on the main thread).
    bool gpuReady = false;
    UINT width = 0, height = 0;
    IDirect3DTexture9* tex[P2R_SLOTS] = {};
    IDirect3DSurface9* surf[P2R_SLOTS] = {};
    IDirect3DQuery9* query[P2R_SLOTS] = {};
    int pendingFirst = 0, pendingCount = 0; // frames copied but not yet published (in order)

    // Encoder process and IPC.
    HANDLE mapping = nullptr;
    P2RShared* shm = nullptr;
    HANDLE semReady = nullptr, semFree = nullptr, evStarted = nullptr;
    HANDLE process = nullptr;

    unsigned id = 0;        // names the kernel objects; passed to the encoder
    int frames = 0;         // frames captured
    bool sawDemo = false;
    DWORD startTick = 0;
    wchar_t outPath[MAX_PATH], wavPath[MAX_PATH];
};

static HMODULE g_self;
static Session g_s;
static unsigned g_counter; // sessions started in this process

// ---------------------------------------------------------------------------------------------

static void ReleaseGpu(Session& s)
{
    for (int i = 0; i < P2R_SLOTS; ++i) {
        if (s.query[i]) s.query[i]->Release();
        if (s.surf[i]) s.surf[i]->Release();
        if (s.tex[i]) s.tex[i]->Release();
        s.query[i] = nullptr;
        s.surf[i] = nullptr;
        s.tex[i] = nullptr;
    }
    s.gpuReady = false;
}

static void CloseIpc(Session& s)
{
    if (s.shm) UnmapViewOfFile(s.shm);
    HANDLE* handles[] = {&s.mapping, &s.semReady, &s.semFree, &s.evStarted, &s.process};
    for (HANDLE* h : handles) {
        if (*h) CloseHandle(*h);
        *h = nullptr;
    }
    s.shm = nullptr;
}

static void RestoreCvars(Session& s)
{
    char cmd[256];
    sprintf_s(cmd, "host_framerate %s; mat_queue_mode %s; fps_max %s", s.hostFramerate, s.queueMode, s.fpsMax);
    engine::Run(cmd);
}

static bool EncoderAlive(Session& s)
{
    return s.process && WaitForSingleObject(s.process, 0) == WAIT_TIMEOUT;
}

// Publishes, in order, the copied frames whose GPU copy has completed. With wait, spins until all
// are published (end of the movie).
static void PublishFrames(Session& s, bool wait)
{
    IDirect3DDevice9Ex* dev = d3d9ex::Device();
    while (s.pendingCount > 0) {
        int slot = s.pendingFirst % P2R_SLOTS;
        HRESULT hr = s.query[slot]->GetData(nullptr, 0, D3DGETDATA_FLUSH);
        if (hr == S_FALSE) {
            if (!wait) return;
            YieldProcessor();
            continue;
        }
        if (hr != S_OK && hr != D3DERR_DEVICELOST) LOG("event query GetData 0x%08lx", hr);
        ReleaseSemaphore(s.semReady, 1, nullptr);
        ++s.pendingFirst;
        --s.pendingCount;
    }
    (void)dev;
}

static void Fail(Session& s, const char* why);

static bool CreateGpu(Session& s)
{
    IDirect3DDevice9Ex* dev = d3d9ex::Device();
    if (!dev) return false;
    IDirect3DSurface9* bb = nullptr;
    D3DSURFACE_DESC d = {};
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    bb->GetDesc(&d);
    bb->Release();
    s.width = d.Width;
    s.height = d.Height;
    for (int i = 0; i < P2R_SLOTS; ++i) {
        HANDLE h = nullptr;
        HRESULT hr = dev->CreateTexture(s.width, s.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                        D3DPOOL_DEFAULT, &s.tex[i], &h);
        if (FAILED(hr) || !h) {
            LOG("shared texture %d: 0x%08lx", i, hr);
            return false;
        }
        s.tex[i]->GetSurfaceLevel(0, &s.surf[i]);
        if (FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &s.query[i]))) return false;
        s.shm->slot_handles[i] = (uint64_t)(uintptr_t)h;
    }
    s.gpuReady = true;
    LOG("shared ring: %d x %ux%u A8R8G8B8 (back buffer fmt %d, ms %d)", P2R_SLOTS, s.width, s.height, d.Format, d.MultiSampleType);
    return true;
}

static bool StartEncoder(Session& s)
{
    P2RShared* m = s.shm;
    m->magic = P2R_MAGIC;
    m->version = P2R_VERSION;
    m->width = s.width;
    m->height = s.height;
    m->fps = s.opt.fps;
    m->codec = s.opt.codec;
    int kbps = s.opt.bitrateKbps;
    if (kbps <= 0) {
        // ~0.33 bits per pixel at 60 fps: 1080p60 -> 40 Mbit/s.
        double px = (double)s.width * s.height * s.opt.fps;
        kbps = (int)(px * 0.33 / 1000.0);
        if (kbps < 8000) kbps = 8000;
    }
    m->bitrate_kbps = kbps;
    m->num_slots = P2R_SLOTS;
    wcscpy_s(m->out_path, s.outPath);
    wcscpy_s(m->wav_path, s.wavPath);

    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_self, exe, MAX_PATH);
    while (n > 0 && exe[n - 1] != L'\\') --n;
    wcscpy_s(exe + n, MAX_PATH - n, L"p2r_encoder.exe");
    wchar_t cmd[MAX_PATH + 64];
    swprintf_s(cmd, L"\"%s\" %u", exe, s.id);
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW | ABOVE_NORMAL_PRIORITY_CLASS,
                        nullptr, nullptr, &si, &pi)) {
        LOG("CreateProcess %ls failed (%lu)", exe, GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    s.process = pi.hProcess;
    HANDLE waits[2] = {s.evStarted, s.process};
    DWORD w = WaitForMultipleObjects(2, waits, FALSE, 15000);
    if (w != WAIT_OBJECT_0 || m->enc_state != P2R_ENC_RUNNING) {
        LOG("encoder did not start (wait %lu, state %d): %s", w, m->enc_state, m->error);
        return false;
    }
    LOG("encoder started: %ux%u %d fps codec %u %d kbit/s", s.width, s.height, s.opt.fps, m->codec, kbps);
    return true;
}

static void Fail(Session& s, const char* why)
{
    engine::Warn("render aborted: %s", why);
    if (s.state == State::Recording) engine::Run("endmovie");
    if (s.state != State::Idle) RestoreCvars(s);
    if (s.process && EncoderAlive(s)) TerminateProcess(s.process, 1);
    ReleaseGpu(s);
    CloseIpc(s);
    s.state = State::Idle;
}

// Replaces CVideoMode::WriteMovieFrame while we render (main thread, before Present).
static bool OnMovieFrame()
{
    Session& s = g_s;
    if (s.state == State::Idle) return false;    // a regular startmovie: engine behaviour
    if (s.state != State::Recording) return true; // finishing: drop the last engine frames

    if (!s.gpuReady) {
        if (!CreateGpu(s) || !StartEncoder(s)) {
            Fail(s, s.gpuReady ? "the encoder did not start (see d3d9ex.log / p2r_encoder.log)" : "cannot create the shared textures");
            return true;
        }
        s.startTick = GetTickCount();
    }

    PublishFrames(s, false);
    // Wait for a free slot: back-pressure when the encoder is slower than the game.
    HANDLE waits[2] = {s.semFree, s.process};
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
        Fail(s, "the encoder exited");
        return true;
    }
    IDirect3DDevice9Ex* dev = d3d9ex::Device();
    int slot = s.frames % P2R_SLOTS;
    IDirect3DSurface9* bb = nullptr;
    HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (SUCCEEDED(hr)) {
        hr = dev->StretchRect(bb, nullptr, s.surf[slot], nullptr, D3DTEXF_NONE);
        bb->Release();
    }
    if (FAILED(hr)) LOG("frame %d: back buffer copy 0x%08lx", s.frames, hr);
    s.query[slot]->Issue(D3DISSUE_END);
    ++s.pendingCount;
    ++s.frames;
    return true;
}

static void Finish(Session& s, const char* why)
{
    engine::Run("endmovie");
    RestoreCvars(s);
    if (!s.gpuReady) {
        engine::Warn("no frame was rendered (%s)", why);
        if (s.process && EncoderAlive(s)) TerminateProcess(s.process, 1);
        CloseIpc(s);
        s.state = State::Idle;
        return;
    }
    PublishFrames(s, true);
    s.shm->total_frames = s.frames;
    s.shm->ended = 1;
    ReleaseSemaphore(s.semReady, 1, nullptr); // wake the encoder even with no pending frame
    DWORD ms = GetTickCount() - s.startTick;
    double secs = ms / 1000.0;
    engine::Msg("%s: %d frames (%.1f s of video) rendered in %.1f s = %.0f fps; encoding / muxing continues",
                why, s.frames, (double)s.frames / s.opt.fps, secs, secs > 0 ? s.frames / secs : 0.0);
    s.state = State::Finishing;
}

// Main thread, FRAME_START.
static void OnFrame()
{
    Session& s = g_s;
    switch (s.state) {
    case State::Idle:
        break;
    case State::Arming:
        // mat_queue_mode 0 is applied at the end of a frame: the device must only be used from the
        // main thread before the movie starts.
        if (++s.armFrames == 3) {
            char cmd[512];
            char base[200];
            sprintf_s(base, "p2r_tmp_%s", s.opt.name);
            sprintf_s(cmd, "startmovie \"%s\" wav", base);
            engine::Run(cmd);
            if (s.opt.demo[0]) {
                sprintf_s(cmd, "playdemo \"%s\"", s.opt.demo);
                engine::Run(cmd);
            }
            s.state = State::Recording;
        }
        break;
    case State::Recording:
        if (engine::IsPlayingDemo()) {
            s.sawDemo = true;
        } else if (s.sawDemo) {
            Finish(s, "demo finished");
        }
        if (s.process && s.gpuReady && !EncoderAlive(s)) Fail(s, "the encoder exited");
        break;
    case State::Finishing: {
        int st = s.shm->enc_state;
        bool alive = EncoderAlive(s);
        if (st == P2R_ENC_DONE) {
            engine::Msg("video written: %ls", s.outPath);
        } else if (st == P2R_ENC_FAILED || !alive) {
            engine::Warn("encoder failed: %s", s.shm->error[0] ? s.shm->error : "exited");
        } else {
            break;
        }
        ReleaseGpu(s);
        CloseIpc(s);
        s.state = State::Idle;
        break;
    }
    }
}

// ---------------------------------------------------------------------------------------------

static bool ParseOptions(const CCommand& args, Options& o)
{
    if (args.ArgC() < 2) return false;
    strncpy_s(o.name, args.Arg(1), _TRUNCATE);
    for (char* c = o.name; *c; ++c)
        if (strchr("\\/:*?\"<>| ", *c)) *c = '_';
    for (int i = 2; i < args.ArgC(); ++i) {
        const char* a = args.Arg(i);
        // The engine tokenizer splits "key=value" into "key", "=", "value".
        char key[64] = {}, val[260] = {};
        const char* eq = strchr(a, '=');
        if (eq) {
            strncpy_s(key, a, eq - a);
            strncpy_s(val, eq + 1, _TRUNCATE);
        } else {
            strncpy_s(key, a, _TRUNCATE);
            if (i + 1 < args.ArgC() && strcmp(args.Arg(i + 1), "=") == 0) {
                strncpy_s(val, args.Arg(i + 2), _TRUNCATE);
                i += 2;
            }
        }
        if (!_stricmp(key, "fps")) o.fps = atoi(val);
        else if (!_stricmp(key, "codec")) o.codec = !_stricmp(val, "hevc") || !_stricmp(val, "h265") ? P2R_HEVC : P2R_H264;
        else if (!_stricmp(key, "bitrate")) o.bitrateKbps = atoi(val);
        else if (!_stricmp(key, "window")) o.window = atoi(val) != 0;
        else if (!_stricmp(key, "demo")) strncpy_s(o.demo, val, _TRUNCATE);
        else {
            engine::Warn("unknown option '%s'", key);
            return false;
        }
    }
    if (o.fps < 1 || o.fps > 1000) o.fps = 60;
    return true;
}

static void Copy(char* dst, size_t n, const char* src) { strncpy_s(dst, n, src ? src : "0", _TRUNCATE); }

static bool OpenIpc(Session& s)
{
    wchar_t name[64];
    unsigned id = s.id = GetCurrentProcessId() * 1000 + (++g_counter % 1000);
    swprintf_s(name, L"Local\\p2r_%u_mem", id);
    s.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(P2RShared), name);
    if (!s.mapping) return false;
    s.shm = (P2RShared*)MapViewOfFile(s.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(P2RShared));
    if (!s.shm) return false;
    memset(s.shm, 0, sizeof(P2RShared));
    swprintf_s(name, L"Local\\p2r_%u_ready", id);
    s.semReady = CreateSemaphoreW(nullptr, 0, 0x7fffffff, name);
    swprintf_s(name, L"Local\\p2r_%u_free", id);
    s.semFree = CreateSemaphoreW(nullptr, P2R_SLOTS, P2R_SLOTS, name);
    swprintf_s(name, L"Local\\p2r_%u_started", id);
    s.evStarted = CreateEventW(nullptr, TRUE, FALSE, name);
    return s.semReady && s.semFree && s.evStarted;
}

static void Cmd_Start(const CCommand& args)
{
    Session& s = g_s;
    if (s.state != State::Idle) {
        engine::Warn("already rendering (p2r_stop first)");
        return;
    }
    Options o;
    if (!ParseOptions(args, o)) {
        engine::Msg("usage: p2r_start <name> [demo=<demo>] [fps=60] [codec=h264|hevc] [bitrate=<kbit/s>] [window=1|0]");
        engine::Msg("  renders the engine's movie frames (startmovie) to <Portal 2>\\p2r_movies\\<name>.mp4 with the");
        engine::Msg("  GPU encoder; with demo=, plays the demo and stops at its end, otherwise use p2r_stop.");
        return;
    }
    if (!d3d9ex::Device()) {
        engine::Warn("the game is not running on D3D9Ex (launched with -nod3d9ex?)");
        return;
    }
    if (!engine::MovieHookReady()) {
        engine::Warn("the engine movie hook is not installed (see d3d9ex.log)");
        return;
    }
    s = Session();
    s.opt = o;

    // Output paths.
    wchar_t root[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, root, MAX_PATH);
    while (n > 0 && root[n - 1] != L'\\') --n;
    root[n] = 0;
    swprintf_s(s.outPath, L"%sp2r_movies", root);
    CreateDirectoryW(s.outPath, nullptr);
    swprintf_s(s.outPath, L"%sp2r_movies\\%hs.mp4", root, o.name);
    swprintf_s(s.wavPath, L"%hs\\p2r_tmp_%hs.wav", engine::GameDir(), o.name);
    DeleteFileW(s.wavPath); // startmovie refuses to overwrite it

    if (!OpenIpc(s)) {
        engine::Warn("cannot create the shared memory (%lu)", GetLastError());
        CloseIpc(s);
        return;
    }
    Copy(s.queueMode, sizeof(s.queueMode), engine::GetCvarString("mat_queue_mode"));
    Copy(s.hostFramerate, sizeof(s.hostFramerate), engine::GetCvarString("host_framerate"));
    Copy(s.fpsMax, sizeof(s.fpsMax), engine::GetCvarString("fps_max"));
    char cmd[256];
    sprintf_s(cmd, "mat_queue_mode 0; fps_max 0; host_framerate %d", o.fps);
    engine::Run(cmd);
    s.state = State::Arming;
    engine::Msg("rendering to %ls (%d fps%s)", s.outPath, o.fps, o.window ? "" : ", window not updated");
}

static void Cmd_Stop(const CCommand&)
{
    Session& s = g_s;
    if (s.state == State::Recording) Finish(s, "stopped");
    else if (s.state == State::Arming) Fail(s, "stopped before the first frame");
    else engine::Msg("not rendering");
}

static void OnEngineReady()
{
    engine::SetMovieFrameCallback(OnMovieFrame);
    engine::SetFrameCallback(OnFrame);
    engine::AddCommand("p2r_start", "Render the engine movie (startmovie) with the GPU encoder. p2r_start for usage.", Cmd_Start);
    engine::AddCommand("p2r_stop", "Stop a render started with p2r_start.", Cmd_Stop);
    engine::Msg("p2r ready (D3D9Ex %s): p2r_start <name> [demo=<demo>] [fps=60] ...", d3d9ex::Device() ? "on" : "OFF");
}

void Init(HMODULE self) { g_self = self; }

void OnPresentHooked()
{
    static bool done;
    if (done) return;
    engine::SetInitCallback(OnEngineReady);
    done = engine::TryHookFrame();
}

bool SkipPresent()
{
    return g_s.state == State::Recording && g_s.gpuReady && !g_s.opt.window;
}

} // namespace capture
