// p2r_encoder.exe (x64): started by the game's d3d9.dll proxy for one render. Opens the game's
// shared D3D9Ex textures with D3D11, copies each frame on the GPU and hands it to a Media
// Foundation sink writer with hardware encoding (NVENC / AMF / QuickSync MFTs), then muxes the
// engine's WAV into the final MP4. No frame ever goes through the CPU.
//
//   p2r_encoder.exe <id>      (kernel objects Local\p2r_<id>_*, see src/shared.h)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#include "../src/shared.h"

template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { if (p) p->Release(); }
    void Reset() { if (p) p->Release(); p = nullptr; }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
};

static FILE* g_log;
static P2RShared* g_shm;

static void Log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_log) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        vfprintf(g_log, fmt, ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    va_end(ap);
}

static int Fail(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log("ERROR: %s", buf);
    if (g_shm) {
        strcpy_s(g_shm->error, buf);
        g_shm->enc_state = P2R_ENC_FAILED;
    }
    return 1;
}

#define CHECK(expr)                                                                   \
    do {                                                                              \
        HRESULT hr_ = (expr);                                                         \
        if (FAILED(hr_)) return Fail("%s failed: 0x%08lx (line %d)", #expr, hr_, __LINE__); \
    } while (0)

// ---------------------------------------------------------------------------------------------
// Video: shared textures -> sink writer (temporary video-only MP4)

struct Video {
    Com<ID3D11Device> dev;
    Com<ID3D11DeviceContext> ctx;
    Com<ID3D11Query> query;
    Com<ID3D11Texture2D> slots[P2R_SLOTS];
    Com<IMFDXGIDeviceManager> mgr;
    Com<IMFSinkWriter> writer;
    UINT resetToken = 0;
    DWORD stream = 0;
};

static int InitVideo(Video& v, const wchar_t* path)
{
    P2RShared* m = g_shm;
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &v.dev, nullptr, &v.ctx));
    {
        Com<ID3D10Multithread> mt;
        if (SUCCEEDED(v.dev->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt))) mt->SetMultithreadProtected(TRUE);
    }
    for (UINT i = 0; i < m->num_slots && i < P2R_SLOTS; ++i)
        CHECK(v.dev->OpenSharedResource((HANDLE)(uintptr_t)m->slot_handles[i], __uuidof(ID3D11Texture2D), (void**)&v.slots[i]));
    D3D11_TEXTURE2D_DESC td;
    v.slots[0]->GetDesc(&td);
    Log("shared textures: %u x %ux%u format %d bind 0x%x", m->num_slots, td.Width, td.Height, td.Format, td.BindFlags);
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    CHECK(v.dev->CreateQuery(&qd, &v.query));

    CHECK(MFCreateDXGIDeviceManager(&v.resetToken, &v.mgr));
    CHECK(v.mgr->ResetDevice(v.dev, v.resetToken));

    Com<IMFAttributes> attr;
    CHECK(MFCreateAttributes(&attr, 4));
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attr->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, v.mgr);
    attr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    CHECK(MFCreateSinkWriterFromURL(path, nullptr, attr, &v.writer));

    Com<IMFMediaType> out;
    CHECK(MFCreateMediaType(&out));
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, m->codec == P2R_HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, m->bitrate_kbps * 1000);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (m->codec == P2R_H264) out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    MFSetAttributeSize(out, MF_MT_FRAME_SIZE, m->width, m->height);
    MFSetAttributeRatio(out, MF_MT_FRAME_RATE, m->fps, 1);
    MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    CHECK(v.writer->AddStream(out, &v.stream));

    Com<IMFMediaType> in;
    CHECK(MFCreateMediaType(&in));
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in, MF_MT_FRAME_SIZE, m->width, m->height);
    MFSetAttributeRatio(in, MF_MT_FRAME_RATE, m->fps, 1);
    MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    HRESULT hr = v.writer->SetInputMediaType(v.stream, in, nullptr);
    if (FAILED(hr)) {
        Log("ARGB32 input refused (0x%08lx), trying RGB32", hr);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        CHECK(v.writer->SetInputMediaType(v.stream, in, nullptr));
    }
    CHECK(v.writer->BeginWriting());
    Log("sink writer ready: %ux%u %u fps, %s, %u kbit/s -> %ls", m->width, m->height, m->fps,
        m->codec == P2R_HEVC ? "HEVC" : "H.264", m->bitrate_kbps, path);
    return 0;
}

// Copies the slot into a texture owned by the sample: the slot is free again as soon as the GPU
// copy is done, the sink writer keeps its texture as long as the encoder needs it.
static int EncodeFrame(Video& v, int index)
{
    P2RShared* m = g_shm;
    ID3D11Texture2D* slot = v.slots[index % m->num_slots];
    D3D11_TEXTURE2D_DESC td;
    slot->GetDesc(&td);
    td.MiscFlags = 0;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    Com<ID3D11Texture2D> tex;
    CHECK(v.dev->CreateTexture2D(&td, nullptr, &tex));
    v.ctx->CopyResource(tex, slot);
    v.ctx->End(v.query);
    while (v.ctx->GetData(v.query, nullptr, 0, 0) == S_FALSE) YieldProcessor();

    Com<IMFMediaBuffer> buf;
    CHECK(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), tex, 0, FALSE, &buf));
    {
        Com<IMF2DBuffer> b2;
        DWORD len = 0;
        if (SUCCEEDED(buf->QueryInterface(__uuidof(IMF2DBuffer), (void**)&b2)) && SUCCEEDED(b2->GetContiguousLength(&len)))
            buf->SetCurrentLength(len);
    }
    Com<IMFSample> sample;
    CHECK(MFCreateSample(&sample));
    CHECK(sample->AddBuffer(buf));
    LONGLONG t0 = (LONGLONG)index * 10000000 / m->fps;
    LONGLONG t1 = (LONGLONG)(index + 1) * 10000000 / m->fps;
    sample->SetSampleTime(t0);
    sample->SetSampleDuration(t1 - t0);
    CHECK(v.writer->WriteSample(v.stream, sample));
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Mux: video-only MP4 (copied as is) + engine WAV (encoded to AAC) -> final MP4

static bool WaitFileClosed(const wchar_t* path, DWORD timeoutMs)
{
    DWORD start = GetTickCount();
    for (;;) {
        HANDLE h = CreateFileW(path, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return true;
        }
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            if (GetTickCount() - start > 3000) return false; // never created
        }
        if (GetTickCount() - start > timeoutMs) return false;
        Sleep(50);
    }
}

static int Mux(const wchar_t* videoPath, const wchar_t* wavPath, const wchar_t* outPath)
{
    Com<IMFSourceReader> vr, ar;
    CHECK(MFCreateSourceReaderFromURL(videoPath, nullptr, &vr));
    CHECK(MFCreateSourceReaderFromURL(wavPath, nullptr, &ar));
    const DWORD VS = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, AS = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    Com<IMFMediaType> vtype, pcm;
    CHECK(vr->GetNativeMediaType(VS, 0, &vtype));
    CHECK(ar->GetCurrentMediaType(AS, &pcm));
    UINT32 rate = MFGetAttributeUINT32(pcm, MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
    UINT32 chans = MFGetAttributeUINT32(pcm, MF_MT_AUDIO_NUM_CHANNELS, 2);
    Log("mux: wav %u Hz, %u channels", rate, chans);

    Com<IMFAttributes> attr;
    CHECK(MFCreateAttributes(&attr, 1));
    attr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    Com<IMFSinkWriter> w;
    CHECK(MFCreateSinkWriterFromURL(outPath, nullptr, attr, &w));
    DWORD vs = 0, as = 0;
    CHECK(w->AddStream(vtype, &vs));
    CHECK(w->SetInputMediaType(vs, vtype, nullptr)); // compressed in = out: copied
    Com<IMFMediaType> aac;
    CHECK(MFCreateMediaType(&aac));
    aac->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    aac->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    aac->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    aac->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    aac->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, chans);
    aac->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000); // 192 kbit/s
    CHECK(w->AddStream(aac, &as));
    CHECK(w->SetInputMediaType(as, pcm, nullptr));
    CHECK(w->BeginWriting());

    bool vEnd = false, aEnd = false;
    LONGLONG vNext = 0, aNext = 0;
    Com<IMFSample> vPend, aPend; // read ahead, written in timestamp order
    auto readOne = [](IMFSourceReader* r, DWORD stream, IMFSample** s, LONGLONG* t, bool* end) -> HRESULT {
        for (;;) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            HRESULT hr = r->ReadSample(stream, 0, nullptr, &flags, &ts, s);
            if (FAILED(hr)) return hr;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                *end = true;
                return S_OK;
            }
            if (*s) {
                *t = ts;
                return S_OK;
            }
        }
    };
    CHECK(readOne(vr, VS, &vPend, &vNext, &vEnd));
    CHECK(readOne(ar, AS, &aPend, &aNext, &aEnd));
    while (!vEnd || !aEnd) {
        if (!vEnd && (aEnd || vNext <= aNext)) {
            CHECK(w->WriteSample(vs, vPend));
            vPend.Reset();
            CHECK(readOne(vr, VS, &vPend, &vNext, &vEnd));
        } else {
            CHECK(w->WriteSample(as, aPend));
            aPend.Reset();
            CHECK(readOne(ar, AS, &aPend, &aNext, &aEnd));
        }
    }
    CHECK(w->Finalize());
    return 0;
}

// ---------------------------------------------------------------------------------------------

static int Run(unsigned id)
{
    wchar_t name[64];
    swprintf_s(name, L"Local\\p2r_%u_mem", id);
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!map) {
        Log("no shared memory %ls", name);
        return 1;
    }
    g_shm = (P2RShared*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(P2RShared));
    if (!g_shm || g_shm->magic != P2R_MAGIC || g_shm->version != P2R_VERSION) {
        Log("bad shared memory");
        return 1;
    }
    P2RShared* m = g_shm;
    swprintf_s(name, L"Local\\p2r_%u_ready", id);
    HANDLE semReady = OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, name);
    swprintf_s(name, L"Local\\p2r_%u_free", id);
    HANDLE semFree = OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, name);
    swprintf_s(name, L"Local\\p2r_%u_started", id);
    HANDLE evStarted = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, id / 1000);
    if (!semReady || !semFree || !evStarted || !game) return Fail("cannot open the synchronisation objects");

    wchar_t videoPath[540];
    swprintf_s(videoPath, L"%s.video.mp4", m->out_path);
    DeleteFileW(videoPath);

    CHECK(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    int frames = 0;
    {
        Video v;
        if (InitVideo(v, videoPath)) {
            SetEvent(evStarted);
            return 1;
        }
        m->enc_state = P2R_ENC_RUNNING;
        SetEvent(evStarted);

        DWORD t0 = GetTickCount();
        HANDLE waits[2] = {semReady, game};
        for (;;) {
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0) return Fail("the game exited during the render");
            if (m->ended && frames >= m->total_frames) break;
            if (EncodeFrame(v, frames)) return 1;
            ReleaseSemaphore(semFree, 1, nullptr);
            m->frames_encoded = ++frames;
            if (frames % 600 == 0) Log("%d frames", frames);
        }
        m->enc_state = P2R_ENC_MUXING;
        CHECK(v.writer->Finalize());
        DWORD ms = GetTickCount() - t0;
        Log("video done: %d frames in %.1f s (%.0f fps including waits for the game)", frames, ms / 1000.0,
            ms ? frames * 1000.0 / ms : 0.0);
    }

    if (frames == 0) {
        DeleteFileW(videoPath);
        return Fail("no frame");
    }
    DeleteFileW(m->out_path);
    if (WaitFileClosed(m->wav_path, 30000)) {
        DWORD t0 = GetTickCount();
        if (Mux(videoPath, m->wav_path, m->out_path) == 0) {
            Log("muxed with %ls in %.1f s", m->wav_path, (GetTickCount() - t0) / 1000.0);
            DeleteFileW(videoPath);
            DeleteFileW(m->wav_path);
        } else {
            Log("mux failed, keeping the video without sound");
            DeleteFileW(m->out_path);
            MoveFileW(videoPath, m->out_path);
            m->error[0] = 0;
        }
    } else {
        Log("no WAV (%ls): video without sound", m->wav_path);
        MoveFileW(videoPath, m->out_path);
    }
    MFShutdown();
    m->enc_state = P2R_ENC_DONE;
    Log("done: %ls", m->out_path);
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    wchar_t logPath[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, logPath, MAX_PATH);
    while (n > 0 && logPath[n - 1] != L'\\') --n;
    wcscpy_s(logPath + n, MAX_PATH - n, L"p2r_encoder.log");
    _wfopen_s(&g_log, logPath, L"w");
    if (argc < 2) {
        Log("usage: p2r_encoder <id> (started by the game)");
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int r = Run(wcstoul(argv[1], nullptr, 10));
    if (g_shm && g_shm->enc_state != P2R_ENC_DONE && g_shm->enc_state != P2R_ENC_FAILED) Fail("exited early");
    return r;
}
