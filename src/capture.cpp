#include "capture.h"

#include <d3d9.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "control.h"
#include "d3d9ex.h"
#include "engine.h"
#include "log.h"
#include "shared.h"

namespace capture {

enum class State { Idle, Arming, Recording, Finishing };

struct Session {
    State state = State::Idle;
    P2RJob job = {};
    int jobId = 0;
    int armFrames = 0;
    bool cancelled = false;

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
    DWORD armTick = 0;
    int readyFrames = 0;
    int notPlayingFrames = 0;
    DWORD startTick = 0;
    wchar_t wavPath[MAX_PATH];
};

static HMODULE g_self;
static Session g_s;
static unsigned g_counter;   // sessions started in this process

// Control block shared with P2Render.exe.
static HANDLE g_ctrlMap;
static P2RControl* g_ctrl;
static int g_lastSeq;

// ---------------------------------------------------------------------------------------------
// Reporting to the app

static void SetJob(P2RJobState st, const char* fmt = nullptr, ...)
{
    if (!g_ctrl) return;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(g_ctrl->job_message, sizeof(g_ctrl->job_message), fmt, ap);
        va_end(ap);
        LOG("job %d: %s", g_s.jobId, g_ctrl->job_message);
    }
    g_ctrl->job_state = st;
}

static void SetCore(P2RCoreState st, const char* msg)
{
    if (!g_ctrl) return;
    strcpy_s(g_ctrl->core_message, msg);
    g_ctrl->core_state = st;
}

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
    while (s.pendingCount > 0) {
        int slot = s.pendingFirst % P2R_SLOTS;
        HRESULT hr = s.query[slot]->GetData(nullptr, 0, D3DGETDATA_FLUSH);
        if (hr == S_FALSE) {
            if (!wait) return;
            YieldProcessor();
            continue;
        }
        if (hr != S_OK) LOG("event query GetData 0x%08lx", hr);
        ReleaseSemaphore(s.semReady, 1, nullptr);
        ++s.pendingFirst;
        --s.pendingCount;
    }
}

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
    if (g_ctrl) {
        g_ctrl->width = s.width;
        g_ctrl->height = s.height;
    }
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
    m->fps = s.job.fps;
    m->codec = s.job.codec;
    int kbps = s.job.bitrate_kbps;
    if (kbps <= 0) {
        // ~0.33 bits per pixel: 1080p60 -> 41 Mbit/s.
        double px = (double)s.width * s.height * s.job.fps;
        kbps = (int)(px * 0.33 / 1000.0);
        if (kbps < 8000) kbps = 8000;
    }
    m->bitrate_kbps = kbps;
    m->num_slots = P2R_SLOTS;
    wcscpy_s(m->out_path, s.job.out_path);
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
    LOG("encoder started: %ux%u %d fps codec %u %d kbit/s", s.width, s.height, s.job.fps, m->codec, kbps);
    return true;
}

static void EndJob(Session& s)
{
    ReleaseGpu(s);
    CloseIpc(s);
    s.state = State::Idle;
    if (g_ctrl) g_ctrl->core_state = CORE_READY;
}

static void Fail(Session& s, const char* why)
{
    if (s.state == State::Recording) engine::Run("endmovie");
    if (s.state != State::Idle) RestoreCvars(s);
    if (s.process && EncoderAlive(s)) TerminateProcess(s.process, 1);
    if (s.state == State::Idle) return;
    engine::Run("disconnect");
    DeleteFileW(s.wavPath);
    SetJob(JOB_FAILED, "%s", why);
    EndJob(s);
}

// Replaces CVideoMode::WriteMovieFrame while we render (main thread, before Present).
static bool OnMovieFrame()
{
    Session& s = g_s;
    if (s.state == State::Idle) return false;    // a regular startmovie: engine behaviour
    if (s.state != State::Recording) return true; // finishing: drop the last engine frames

    if (!s.gpuReady) {
        if (!CreateGpu(s)) {
            Fail(s, "impossible de créer les textures partagées");
            return true;
        }
        if (!StartEncoder(s)) {
            Fail(s, s.shm && s.shm->error[0] ? s.shm->error : "l'encodeur n'a pas démarré");
            return true;
        }
        s.startTick = GetTickCount();
        SetJob(JOB_RECORDING);
    }

    PublishFrames(s, false);
    // Wait for a free slot: back-pressure when the encoder is slower than the game.
    HANDLE waits[2] = {s.semFree, s.process};
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
        Fail(s, s.shm->error[0] ? s.shm->error : "l'encodeur s'est arrêté");
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
    if (g_ctrl) {
        g_ctrl->frames = s.frames;
        g_ctrl->render_ms = GetTickCount() - s.startTick;
    }
    return true;
}

static void Finish(Session& s, const char* why)
{
    engine::Run("endmovie");
    RestoreCvars(s);
    engine::Run("disconnect");
    if (!s.gpuReady) {
        if (s.process && EncoderAlive(s)) TerminateProcess(s.process, 1);
        DeleteFileW(s.wavPath);
        SetJob(JOB_FAILED, "aucune image rendue (%s)", why);
        EndJob(s);
        return;
    }
    PublishFrames(s, true);
    s.shm->total_frames = s.frames;
    s.shm->ended = 1;
    ReleaseSemaphore(s.semReady, 1, nullptr); // wake the encoder even with no pending frame
    DWORD ms = GetTickCount() - s.startTick;
    if (g_ctrl) g_ctrl->render_ms = ms;
    SetJob(JOB_ENCODING, "%s : %d images en %.1f s (%.0f images/s)", why, s.frames, ms / 1000.0,
           ms ? s.frames * 1000.0 / ms : 0.0);
    s.state = State::Finishing;
}

// ---------------------------------------------------------------------------------------------
// Jobs from the app

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

static void StartJob(const P2RJob& job, int jobId)
{
    Session& s = g_s;
    s = Session();
    s.job = job;
    s.jobId = jobId;
    if (s.job.fps < 1 || s.job.fps > 1000) s.job.fps = 60;
    g_ctrl->job_id = jobId;
    g_ctrl->frames = 0;
    g_ctrl->frames_encoded = 0;
    g_ctrl->render_ms = 0;
    g_ctrl->job_message[0] = 0;
    g_ctrl->core_state = CORE_BUSY;
    SetJob(JOB_LOADING, "chargement de la démo");

    swprintf_s(s.wavPath, L"%hs\\p2r_tmp_%d.wav", engine::GameDir(), jobId);
    DeleteFileW(s.wavPath); // startmovie refuses to overwrite it
    if (!OpenIpc(s)) {
        CloseIpc(s);
        SetJob(JOB_FAILED, "mémoire partagée impossible à créer (%lu)", GetLastError());
        g_ctrl->core_state = CORE_READY;
        return;
    }
    Copy(s.queueMode, sizeof(s.queueMode), engine::GetCvarString("mat_queue_mode"));
    Copy(s.hostFramerate, sizeof(s.hostFramerate), engine::GetCvarString("host_framerate"));
    Copy(s.fpsMax, sizeof(s.fpsMax), engine::GetCvarString("fps_max"));
    char cmd[256];
    sprintf_s(cmd, "mat_queue_mode 0; fps_max 0; host_framerate %d", s.job.fps);
    engine::Run(cmd);
    s.state = State::Arming;
    LOG("job %d: %s -> %ls (%d fps, codec %d, %d kbit/s, window %d)", jobId, job.demo, job.out_path, job.fps,
        job.codec, job.bitrate_kbps, job.show_window);
}

static void PollCommands()
{
    if (!g_ctrl || g_ctrl->cmd_seq == g_lastSeq) return;
    int seq = g_ctrl->cmd_seq;
    MemoryBarrier();
    P2RCommand cmd = (P2RCommand)g_ctrl->cmd;
    P2RJob job = g_ctrl->job;
    g_lastSeq = seq;
    g_ctrl->ack_seq = seq;
    switch (cmd) {
    case CMD_RENDER:
        if (g_s.state != State::Idle) LOG("render command ignored: busy");
        else StartJob(job, seq);
        break;
    case CMD_CANCEL:
        if (g_s.state == State::Arming || g_s.state == State::Recording) {
            g_s.cancelled = true;
            Fail(g_s, "annulé");
        }
        break;
    case CMD_QUIT:
        if (g_s.state != State::Idle) Fail(g_s, "annulé");
        engine::Run("quit");
        break;
    default:
        break;
    }
}

// Main thread, FRAME_START.
static void OnFrame()
{
    PollCommands();
    Session& s = g_s;
    switch (s.state) {
    case State::Idle:
        break;
    case State::Arming: {
        // mat_queue_mode 0 is applied at the end of a frame: the device must only be used from the
        // main thread once the movie starts. The movie starts when the demo is in game (not during
        // its map load), so the video and the engine WAV start on the same frame.
        ++s.armFrames;
        char cmd[512];
        if (s.armFrames == 3) {
            sprintf_s(cmd, "playdemo \"%s\"", s.job.demo);
            engine::Run(cmd);
            s.armTick = GetTickCount();
        }
        if (s.armFrames >= 3) {
            if (engine::IsPlayingDemo() && engine::IsInGame() && !engine::IsLoading()) {
                if (++s.readyFrames >= 2) {
                    sprintf_s(cmd, "startmovie \"p2r_tmp_%d\" wav", s.jobId);
                    engine::Run(cmd);
                    s.state = State::Recording;
                }
            } else if (GetTickCount() - s.armTick > 120000) {
                Fail(s, "la démo n'a pas démarré (démo incompatible avec cette version du jeu ?)");
            }
        }
        break;
    }
    case State::Recording:
        // Stop when the demo is over (not playing for a few frames).
        if (s.frames > 0) {
            if (engine::IsPlayingDemo()) {
                s.notPlayingFrames = 0;
            } else if (++s.notPlayingFrames >= 10) {
                Finish(s, "démo terminée");
                break;
            }
        }
        if (s.process && s.gpuReady && !EncoderAlive(s)) Fail(s, s.shm->error[0] ? s.shm->error : "l'encodeur s'est arrêté");
        break;
    case State::Finishing: {
        int st = s.shm->enc_state;
        if (g_ctrl) g_ctrl->frames_encoded = s.shm->frames_encoded;
        bool alive = EncoderAlive(s);
        if (st == P2R_ENC_DONE) {
            SetJob(JOB_DONE, "vidéo écrite : %ls", s.job.out_path);
        } else if (st == P2R_ENC_FAILED || !alive) {
            SetJob(JOB_FAILED, "encodeur : %s", s.shm->error[0] ? s.shm->error : "arrêté");
        } else {
            break;
        }
        EndJob(s);
        break;
    }
    }
    if (g_ctrl && s.shm && s.state == State::Recording) g_ctrl->frames_encoded = s.shm->frames_encoded;
}

// ---------------------------------------------------------------------------------------------

static void OnEngineReady()
{
    engine::SetMovieFrameCallback(OnMovieFrame);
    engine::SetFrameCallback(OnFrame);
    wchar_t name[64];
    swprintf_s(name, L"Local\\p2render_ctrl_%lu", GetCurrentProcessId());
    g_ctrlMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(P2RControl), name);
    g_ctrl = g_ctrlMap ? (P2RControl*)MapViewOfFile(g_ctrlMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(P2RControl)) : nullptr;
    if (!g_ctrl) {
        LOG("cannot create the control block (%lu)", GetLastError());
        return;
    }
    memset(g_ctrl, 0, sizeof(*g_ctrl));
    g_ctrl->magic = P2R_CTRL_MAGIC;
    g_ctrl->version = P2R_CTRL_VERSION;
    if (!d3d9ex::Device()) SetCore(CORE_BROKEN, "le jeu ne tourne pas en Direct3D 9Ex");
    else if (!engine::MovieHookReady()) SetCore(CORE_BROKEN, "point d'accroche du moteur introuvable (version du jeu ?)");
    else SetCore(CORE_READY, "prêt");
    LOG("control block %ls: %s", name, g_ctrl->core_message);
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
    return g_s.state == State::Recording && g_s.gpuReady && !g_s.job.show_window;
}

} // namespace capture
