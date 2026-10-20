// p2r_encoder.exe (x64): started by the game's d3d9.dll proxy for one render. Opens the game's
// shared D3D9Ex textures with D3D11, copies each frame on the GPU and hands it to a Media
// Foundation sink writer with hardware encoding (NVENC / AMF / QuickSync MFTs), then muxes the
// engine's WAV into the final MP4. No frame ever goes through the CPU.
//
//   p2r_encoder.exe <id>      (kernel objects Local\p2r_<id>_*, see src/shared.h)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <stdarg.h>
#include <share.h>
#include <stdio.h>
#include <wchar.h>

#include <algorithm>
#include <string>
#include <vector>

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
    if (m->use_ffmpeg) return 0; // the ffmpeg path (below) uses its own staging textures

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
// Advanced mode: ffmpeg. Each slot is copied into a staging texture, read back and piped to
// ffmpeg as raw BGRA (the only CPU copy of the pipeline, in this x64 process, off the game).

static std::wstring g_ffLog;

// Live sound: the engine WAV is copied into a named pipe while it is written (see PumpAudio).
struct AudioPipe {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE thread = nullptr;
    HANDLE stop = nullptr;
    wchar_t wav[MAX_PATH];
    wchar_t name[64];
    UINT rate = 44100, channels = 2;
    DWORD dataOfs = 44;      // start of the PCM data
    LONG64 sent = 0;         // samples written to the pipe (real + silence)
    LONG64 skip = 0;         // samples of the engine stream already covered by silence
    P2RShared* shm = nullptr;
    int fps = 60;
};

struct Ffmpeg {
    HANDLE proc = nullptr;
    HANDLE in = nullptr;  // write end of ffmpeg's stdin
    Com<ID3D11Texture2D> staging[3];
    std::vector<char> row;
    AudioPipe audio;
    ~Ffmpeg();
};

// Runs ffmpeg with the given arguments; stdin = pipe (when in != null), stderr appended to
// p2r_ffmpeg.log next to the encoder.
static bool StartFfmpeg(const std::wstring& args, HANDLE* in, HANDLE* proc)
{
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (in && !CreatePipe(&rd, &wr, &sa, 64 << 20)) return false;
    if (wr) SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    HANDLE log = CreateFileW(g_ffLog.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, 0, nullptr);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = rd ? rd : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = log;
    si.hStdError = log;
    std::wstring cmd = L"\"" + std::wstring(g_shm->ffmpeg_path) + L"\" " + args;
    Log("ffmpeg %ls", args.c_str());
    if (log != INVALID_HANDLE_VALUE) {
        char utf8[4096] = {};
        WideCharToMultiByte(CP_UTF8, 0, args.c_str(), -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
        std::string line = std::string("\r\n> ffmpeg ") + utf8 + "\r\n";
        DWORD w;
        WriteFile(log, line.data(), (DWORD)line.size(), &w, nullptr);
    }    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(0);
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(g_shm->ffmpeg_path, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                             nullptr, nullptr, &si, &pi);
    if (rd) CloseHandle(rd);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        if (wr) CloseHandle(wr);
        Log("cannot start ffmpeg (%lu): %ls", GetLastError(), g_shm->ffmpeg_path);
        return false;
    }
    CloseHandle(pi.hThread);
    *proc = pi.hProcess;
    if (in) *in = wr;
    return true;
}

// Last non-empty line of p2r_ffmpeg.log, for the error message shown to the user.
static std::string FfmpegLastLine()
{
    FILE* f = _wfsopen(g_ffLog.c_str(), L"rb", _SH_DENYNO);
    if (!f) return "ffmpeg a échoué";
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    fclose(f);
    // The first message of the last ffmpeg run is usually the cause (the next ones are consequences).
    size_t block = all.rfind("\n> ffmpeg");
    size_t pos = block == std::string::npos ? 0 : all.find('\n', block + 1);
    while (pos != std::string::npos && pos < all.size()) {
        size_t start = pos + 1, e = all.find('\n', start);
        std::string l = all.substr(start, (e == std::string::npos ? all.size() : e) - start);
        while (!l.empty() && (l.back() == 13 || l.back() == 10)) l.pop_back();
        // Drop the "[codec @ 0x...] " prefix.
        if (!l.empty() && l[0] == '[' && l.find("] ") != std::string::npos) l = l.substr(l.find("] ") + 2);
        if (!l.empty() && l.find("Guessed Channel Layout") == std::string::npos) return "ffmpeg : " + l.substr(0, 220);
        pos = e;
    }
    return "ffmpeg a échoué";
}

static int WaitFfmpeg(HANDLE proc)
{
    WaitForSingleObject(proc, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    return (int)code;
}

// The engine WAV (startmovie ... wav) exists before the first frame and grows while we render.
// Live sound. ffmpeg cannot follow a growing file (the wav demuxer stops at the first short read)
// and it stops reading the video when the sound starves, so the pump:
//  - copies the WAV data into a named pipe as the engine writes it (a pipe blocks, a file returns EOF),
//    skipping the RIFF header: the raw PCM demuxer never needs a big read to start;
//  - pads with silence whenever the sound falls behind the video, otherwise the whole pipeline
//    deadlocks (ffmpeg waits for sound, the engine waits for free texture slots). The samples sent
//    as silence are dropped from the engine stream when they arrive: the timing stays exact.
static bool WaveFormat(AudioPipe& a, const wchar_t* path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char hdr[44] = {};
    DWORD n = 0;
    BOOL ok = ReadFile(h, hdr, sizeof(hdr), &n, nullptr);
    CloseHandle(h);
    if (!ok || n < 44) return false;
    if (memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4) || memcmp(hdr + 12, "fmt ", 4) || memcmp(hdr + 36, "data", 4))
        return false;
    WORD tag = 0, bits = 0, ch = 0;
    UINT rate = 0;
    memcpy(&tag, hdr + 20, 2);
    memcpy(&ch, hdr + 22, 2);
    memcpy(&rate, hdr + 24, 4);
    memcpy(&bits, hdr + 34, 2);
    if (tag != 1 || bits != 16 || ch < 1 || ch > 8 || rate < 8000 || rate > 192000) return false;
    a.rate = rate;
    a.channels = ch;
    return true;
}

static DWORD WINAPI PumpAudio(LPVOID p)
{
    AudioPipe* a = (AudioPipe*)p;
    HANDLE wav = INVALID_HANDLE_VALUE;
    if (!ConnectNamedPipe(a->pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
        Log("audio pump: ConnectNamedPipe failed (%lu)", GetLastError());
        goto done;
    }
    Log("audio pump: ffmpeg connected, %u Hz %u ch, data at %lu", a->rate, a->channels, a->dataOfs);
    wav = CreateFileW(a->wav, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (wav == INVALID_HANDLE_VALUE) {
        Log("audio pump: cannot read %ls (%lu)", a->wav, GetLastError());
        goto done;
    }
    if (SetFilePointer(wav, a->dataOfs, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) goto done;
    Log("audio pump: copying %ls", a->wav);
    {
        char buf[256 << 10];
        const DWORD align = a->channels * 2;
        LONG64 frameSamples = a->rate / a->fps; // samples of sound per video frame
        auto copy = [&](const char* data, DWORD len) -> bool {
            DWORD off = 0, w = 0;
            while (off < len && WriteFile(a->pipe, data + off, len - off, &w, nullptr)) off += w;
            return off == len;
        };
        for (;;) {
            DWORD n = 0;
            if (ReadFile(wav, buf, sizeof(buf), &n, nullptr) && n) {
                DWORD off = 0;
                while (a->skip > 0 && off < n) { // already sent as silence
                    DWORD drop = (DWORD)std::min<LONG64>(a->skip * align, n - off);
                    drop -= drop % align;
                    off += drop;
                    a->skip -= drop / align;
                }
                if (off < n && !copy(buf + off, n - off)) break;
                a->sent += (n - off) / align;
            }
            // Keep the sound at least as far as the video, or ffmpeg stalls on its audio input and
            // the render deadlocks behind it.
            LONG64 want = (LONG64)(a->shm ? a->shm->frames_encoded : 0) * frameSamples;
            if (a->sent < want) {
                LONG64 pad = std::min<LONG64>(want - a->sent, frameSamples * 30);
                std::vector<char> silence((size_t)pad * align, 0);
                if (!copy(silence.data(), (DWORD)silence.size())) break;
                a->sent += pad;
                a->skip += pad;
                Log("audio pump: %I64d samples of silence (the engine sound lags behind)", pad);
            }
            if (WaitForSingleObject(a->stop, 20) == WAIT_OBJECT_0) break;
        }
        DWORD n = 0;
        while (ReadFile(wav, buf, sizeof(buf), &n, nullptr) && n) // what the engine wrote last
            if (!copy(buf, n)) break;
    }
done:
    if (wav != INVALID_HANDLE_VALUE) CloseHandle(wav);
    Log("audio pump: end, sent %I64d samples, skip %I64d", a->sent, a->skip);
    if (a->pipe != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(a->pipe);
        DisconnectNamedPipe(a->pipe);
    }
    return 0;
}

static void StopAudio(AudioPipe& a)
{
    if (a.stop) SetEvent(a.stop);
    if (a.pipe != INVALID_HANDLE_VALUE) CancelIoEx(a.pipe, nullptr);
    if (a.thread) {
        WaitForSingleObject(a.thread, 10000);
        CloseHandle(a.thread);
        a.thread = nullptr;
    }
    if (a.stop) {
        CloseHandle(a.stop);
        a.stop = nullptr;
    }
    if (a.pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(a.pipe);
        a.pipe = INVALID_HANDLE_VALUE;
    }
}

Ffmpeg::~Ffmpeg() { StopAudio(audio); }

// Creates the pipe and starts the pump before ffmpeg opens it. False when the engine WAV is not
// readable: the caller then streams the video alone.
static bool StartAudio(AudioPipe& a, const wchar_t* wav)
{
    bool ok = false;
    for (int i = 0; i < 20 && !ok; ++i) {
        ok = WaveFormat(a, wav);
        if (!ok) Sleep(100);
    }
    if (!ok) return false;
    wcscpy_s(a.wav, wav);
    a.shm = g_shm;
    a.fps = g_shm->fps ? (int)g_shm->fps : 60;
    swprintf_s(a.name, L"\\\\.\\pipe\\p2r_audio_%lu", GetCurrentProcessId());
    a.pipe = CreateNamedPipeW(a.name, PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                              1, 1 << 20, 0, 0, nullptr);
    if (a.pipe == INVALID_HANDLE_VALUE) {
        Log("audio pump: CreateNamedPipe %lu", GetLastError());
        return false;
    }
    a.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    a.thread = CreateThread(nullptr, 0, PumpAudio, &a, 0, nullptr);
    return a.thread && a.stop;
}

static int InitFfmpeg(Video& v, Ffmpeg& f, const wchar_t* outFile)
{
    P2RShared* m = g_shm;
    D3D11_TEXTURE2D_DESC td;
    v.slots[0]->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    for (auto& s : f.staging) CHECK(v.dev->CreateTexture2D(&td, nullptr, &s));
    wchar_t args[2600];
    if (m->stream) {
        // Live output: the sound is muxed while it is written. No -map: ffmpeg picks one video and
        // (when the pipe is there) one audio stream from the inputs.
        wchar_t audio[600] = L"";
        if (wcsstr(m->ff_video_args, L"-an")) Log("stream without sound (-an)");
        else if (StartAudio(f.audio, m->wav_path))
            // -probesize/-analyzeduration: without them find_stream_info() waits for 5 MB of sound
            // (29 s) before handing over, and the render deadlocks behind the throttled game.
            swprintf_s(audio, L"-f s16le -ar %u -ac %u -probesize 4096 -analyzeduration 0 -i \"%s\" ",
                       f.audio.rate, f.audio.channels, f.audio.name);
        else Log("no live sound (%ls is not readable), streaming the video alone", m->wav_path);
        swprintf_s(args, L"-hide_banner -loglevel warning -y -f rawvideo -pix_fmt bgra -s %ux%u -framerate %u -i pipe:0 %s%s \"%s\"",
                   m->width, m->height, m->fps, audio, m->ff_video_args, outFile);
    } else {
        swprintf_s(args, L"-hide_banner -loglevel warning -y -f rawvideo -pix_fmt bgra -s %ux%u -framerate %u -i pipe:0 %s -an \"%s\"",
                   m->width, m->height, m->fps, m->ff_video_args, outFile);
    }
    if (!StartFfmpeg(args, &f.in, &f.proc)) return Fail("impossible de lancer ffmpeg (%ls)", m->ffmpeg_path);
    return 0;
}

static int EncodeFrameFF(Video& v, Ffmpeg& f, int index, HANDLE semFree)
{
    P2RShared* m = g_shm;
    ID3D11Texture2D* st = f.staging[index % 3];
    v.ctx->CopyResource(st, v.slots[index % m->num_slots]);
    v.ctx->End(v.query);
    while (v.ctx->GetData(v.query, nullptr, 0, 0) == S_FALSE) YieldProcessor();
    ReleaseSemaphore(semFree, 1, nullptr); // the slot is free once copied
    D3D11_MAPPED_SUBRESOURCE map;
    CHECK(v.ctx->Map(st, 0, D3D11_MAP_READ, 0, &map));
    const char* p = (const char*)map.pData;
    DWORD rowBytes = m->width * 4;
    bool ok = true;
    if (map.RowPitch == rowBytes) {
        DWORD total = rowBytes * m->height, off = 0, w = 0;
        while (ok && off < total) {
            ok = WriteFile(f.in, p + off, total - off, &w, nullptr) != 0;
            off += w;
        }
    } else {
        for (UINT y = 0; ok && y < m->height; ++y) {
            DWORD off = 0, w = 0;
            while (ok && off < rowBytes) {
                ok = WriteFile(f.in, p + (size_t)y * map.RowPitch + off, rowBytes - off, &w, nullptr) != 0;
                off += w;
            }
        }
    }
    v.ctx->Unmap(st, 0);
    if (!ok) return Fail("%s", FfmpegLastLine().c_str());
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

    bool ff = m->use_ffmpeg != 0;
    bool stream = ff && m->stream != 0;
    wchar_t videoPath[540];
    if (stream) wcscpy_s(videoPath, m->out_path); // the live stream is written where the final file would be
    else swprintf_s(videoPath, ff ? L"%s.video.mkv" : L"%s.video.mp4", m->out_path);
    DeleteFileW(videoPath);

    CHECK(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    int frames = 0;
    {
        Video v;
        Ffmpeg f;
        if (InitVideo(v, videoPath) || (ff && InitFfmpeg(v, f, videoPath))) {
            SetEvent(evStarted);
            return 1;
        }
        m->enc_state = P2R_ENC_RUNNING;
        SetEvent(evStarted);

        DWORD t0 = GetTickCount();
        HANDLE waits[2] = {semReady, game};
        for (;;) {
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0) return Fail("le jeu s'est fermé pendant le rendu");
            if (m->ended && frames >= m->total_frames) break;
            if (ff) {
                if (EncodeFrameFF(v, f, frames, semFree)) {
                    TerminateProcess(f.proc, 1);
                    return 1;
                }
            } else {
                if (EncodeFrame(v, frames)) return 1;
                ReleaseSemaphore(semFree, 1, nullptr);
            }
            m->frames_encoded = ++frames;
            if (frames % 600 == 0) Log("%d frames", frames);
        }
        m->enc_state = P2R_ENC_MUXING;
        if (ff) {
            CloseHandle(f.in);
            f.in = nullptr;
            StopAudio(f.audio); // end of the sound: ffmpeg finishes the stream
            int code = WaitFfmpeg(f.proc);
            if (code != 0) return Fail("%s", FfmpegLastLine().c_str());
        } else {
            CHECK(v.writer->Finalize());
        }
        DWORD ms = GetTickCount() - t0;
        Log("video done: %d frames in %.1f s (%.0f fps including waits for the game)", frames, ms / 1000.0,
            ms ? frames * 1000.0 / ms : 0.0);
    }

    if (frames == 0) {
        DeleteFileW(videoPath);
        return Fail("no frame");
    }
    if (stream) {
        // The stream is the result: the sound is already muxed in it, nothing to assemble.
        DeleteFileW(m->wav_path);
        MFShutdown();
        m->enc_state = P2R_ENC_DONE;
        Log("stream done: %ls", m->out_path);
        return 0;
    }
    DeleteFileW(m->out_path);
    bool wav = WaitFileClosed(m->wav_path, 30000);
    DWORD t0 = GetTickCount();
    if (ff) {
        // ffmpeg mux: video copied, audio encoded with the chosen options (or the video alone,
        // remuxed into the chosen container).
        wchar_t args[2048];
        if (wav)
            swprintf_s(args, L"-hide_banner -loglevel warning -y -i \"%s\" -i \"%s\" -map 0:v -map 1:a -c:v copy %s \"%s\"",
                       videoPath, m->wav_path, m->ff_audio_args, m->out_path);
        else
            swprintf_s(args, L"-hide_banner -loglevel warning -y -i \"%s\" -c copy \"%s\"", videoPath, m->out_path);
        HANDLE proc = nullptr;
        int code = StartFfmpeg(args, nullptr, &proc) ? WaitFfmpeg(proc) : 1;
        if (code != 0) {
            std::string why = FfmpegLastLine();
            Log("ffmpeg mux failed (%s), keeping the video without sound as .mkv", why.c_str());
            std::wstring keep = std::wstring(m->out_path) + L".mkv";
            DeleteFileW(m->out_path);
            MoveFileExW(videoPath, keep.c_str(), MOVEFILE_REPLACE_EXISTING);
            return Fail("assemblage du son impossible (%s) ; vidéo sans son gardée en .mkv", why.c_str());
        }
        DeleteFileW(videoPath);
        if (wav) DeleteFileW(m->wav_path);
        Log("ffmpeg mux done in %.1f s", (GetTickCount() - t0) / 1000.0);
    } else if (wav) {
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
    g_ffLog = logPath;
    g_ffLog.replace(g_ffLog.size() - 15, 15, L"p2r_ffmpeg.log");
    DeleteFileW(g_ffLog.c_str());
    g_log = _wfsopen(logPath, L"w", _SH_DENYNO);
    if (argc < 2) {
        Log("usage: p2r_encoder <id> (started by the game)");
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int r = Run(wcstoul(argv[1], nullptr, 10));
    if (g_shm && g_shm->enc_state != P2R_ENC_DONE && g_shm->enc_state != P2R_ENC_FAILED) Fail("exited early");
    return r;
}
