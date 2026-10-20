// P2Render.exe (x64): the user-facing application. Serves its interface (ui\index.html) on a local
// HTTP port and shows it in an Edge app window; runs the render queue: starts Portal 2 through
// p2r_launch.exe (p2r_core.dll injected, nothing installed in the game), sends one job per demo
// through the control block (src/control.h) and reports progress to the interface.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <urlmon.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <bcrypt.h>
#include <share.h>
#include <stdio.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/control.h"
#include "../src/shared.h"

using std::string;
using std::wstring;

// ---------------------------------------------------------------------------------------------
// Utilities

static wstring Widen(const string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static string Narrow(const wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static string JsonEsc(const string& s)
{
    string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                sprintf_s(b, "\\u%04x", c);
                o += b;
            } else {
                o += (char)c;
            }
        }
    }
    return o;
}
static string Q(const string& s) { return "\"" + JsonEsc(s) + "\""; }
static string Q(const wstring& s) { return Q(Narrow(s)); }

// Flat JSON lookup ("key": value) - enough for the small objects the interface sends.
static bool JsonRaw(const string& body, const char* key, string& out)
{
    string k = string("\"") + key + "\"";
    size_t p = body.find(k);
    if (p == string::npos) return false;
    p = body.find(':', p + k.size());
    if (p == string::npos) return false;
    ++p;
    while (p < body.size() && isspace((unsigned char)body[p])) ++p;
    if (p >= body.size()) return false;
    if (body[p] == '"') {
        string v;
        for (++p; p < body.size() && body[p] != '"'; ++p) {
            if (body[p] == '\\' && p + 1 < body.size()) {
                ++p;
                char c = body[p];
                if (c == 'n') v += '\n';
                else if (c == 't') v += '\t';
                else if (c == 'u' && p + 4 < body.size()) {
                    unsigned cp = strtoul(body.substr(p + 1, 4).c_str(), nullptr, 16);
                    p += 4;
                    wchar_t wc = (wchar_t)cp;
                    v += Narrow(wstring(1, wc));
                } else v += c;
            } else {
                v += body[p];
            }
        }
        out = v;
        return true;
    }
    size_t e = p;
    while (e < body.size() && body[e] != ',' && body[e] != '}' && !isspace((unsigned char)body[e])) ++e;
    out = body.substr(p, e - p);
    return true;
}
static string JsonStr(const string& b, const char* k, const string& def = "") { string v; return JsonRaw(b, k, v) ? v : def; }
static int JsonInt(const string& b, const char* k, int def = 0) { string v; return JsonRaw(b, k, v) ? atoi(v.c_str()) : def; }
static bool JsonBool(const string& b, const char* k, bool def = false) { string v; return JsonRaw(b, k, v) ? v == "true" || v == "1" : def; }

static wstring ExeDir()
{
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    while (n > 0 && p[n - 1] != L'\\') --n;
    p[n] = 0;
    return p;
}

static wstring KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

static bool FileExists(const wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static wstring FileName(const wstring& p)
{
    size_t s = p.find_last_of(L"\\/");
    return s == wstring::npos ? p : p.substr(s + 1);
}

static wstring StripExt(const wstring& p)
{
    size_t d = p.find_last_of(L'.');
    return d == wstring::npos ? p : p.substr(0, d);
}

static FILE* g_log;
static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------------------------------------
// Settings

struct Settings {
    int width = 1920, height = 1080;
    int fps = 60;
    string codec = "h264";       // h264 | hevc
    string quality = "high";     // standard | high | max
    wstring outDir;
    wstring gameDir;             // empty = found through Steam
    bool showWindow = false;     // false = invisible rendering (game window hidden, game sound muted)
    bool closeGame = true;
    // Advanced mode: encode with ffmpeg (the interface builds the arguments).
    bool advanced = false;
    wstring ffmpegPath;          // empty = auto (downloaded copy, then PATH)
    string container = "mp4";    // output extension
    string ffVideo = "-c:v h264_nvenc -preset p5 -tune hq -rc vbr -cq 19 -b:v 0 -pix_fmt yuv420p"; // = interface default
    string ffAudio = "-c:a aac -b:a 320k";
    string ffUi;                 // the interface's own state of the advanced panel (opaque JSON)
};

static wstring g_dataDir;        // %LOCALAPPDATA%\P2Render
static Settings g_set;
static std::mutex g_mx;          // protects everything below + g_set

static wstring IniPath() { return g_dataDir + L"\\settings.ini"; }

static wstring IniGet(const wchar_t* key, const wstring& def)
{
    wchar_t buf[8192];
    GetPrivateProfileStringW(L"p2render", key, def.c_str(), buf, 8192, IniPath().c_str());
    return buf;
}
static void IniSet(const wchar_t* key, const wstring& v) { WritePrivateProfileStringW(L"p2render", key, v.c_str(), IniPath().c_str()); }

static void LoadSettings()
{
    Settings s;
    s.outDir = KnownFolder(FOLDERID_Videos) + L"\\P2Render";
    s.width = _wtoi(IniGet(L"width", L"1920").c_str());
    s.height = _wtoi(IniGet(L"height", L"1080").c_str());
    s.fps = _wtoi(IniGet(L"fps", L"60").c_str());
    s.codec = Narrow(IniGet(L"codec", L"h264"));
    s.quality = Narrow(IniGet(L"quality", L"high"));
    s.outDir = IniGet(L"outDir", s.outDir);
    s.gameDir = IniGet(L"gameDir", L"");
    s.showWindow = IniGet(L"showWindow", L"0") == L"1";
    s.closeGame = IniGet(L"closeGame", L"1") == L"1";
    s.advanced = IniGet(L"advanced", L"0") == L"1";
    s.ffmpegPath = IniGet(L"ffmpegPath", L"");
    s.container = Narrow(IniGet(L"container", Widen(s.container)));
    s.ffVideo = Narrow(IniGet(L"ffVideo", Widen(s.ffVideo)));
    s.ffAudio = Narrow(IniGet(L"ffAudio", Widen(s.ffAudio)));
    s.ffUi = Narrow(IniGet(L"ffUi", L""));
    if (s.width < 320 || s.height < 240) s.width = 1920, s.height = 1080;
    if (s.fps < 1 || s.fps > 1000) s.fps = 60;
    g_set = s;
}

static void SaveSettings()
{
    const Settings& s = g_set;
    IniSet(L"width", std::to_wstring(s.width));
    IniSet(L"height", std::to_wstring(s.height));
    IniSet(L"fps", std::to_wstring(s.fps));
    IniSet(L"codec", Widen(s.codec));
    IniSet(L"quality", Widen(s.quality));
    IniSet(L"outDir", s.outDir);
    IniSet(L"gameDir", s.gameDir);
    IniSet(L"showWindow", s.showWindow ? L"1" : L"0");
    IniSet(L"closeGame", s.closeGame ? L"1" : L"0");
    IniSet(L"advanced", s.advanced ? L"1" : L"0");
    IniSet(L"ffmpegPath", s.ffmpegPath);
    IniSet(L"container", Widen(s.container));
    IniSet(L"ffVideo", Widen(s.ffVideo));
    IniSet(L"ffAudio", Widen(s.ffAudio));
    IniSet(L"ffUi", Widen(s.ffUi));
}

// ---------------------------------------------------------------------------------------------
// Portal 2 location (Steam libraries)

static wstring FindPortal2()
{
    HKEY k;
    wchar_t steam[MAX_PATH] = {};
    DWORD sz = sizeof(steam);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegQueryValueExW(k, L"SteamPath", nullptr, nullptr, (BYTE*)steam, &sz);
        RegCloseKey(k);
    }
    std::vector<wstring> libs;
    if (steam[0]) {
        wstring s = steam;
        std::replace(s.begin(), s.end(), L'/', L'\\');
        libs.push_back(s);
        FILE* f = nullptr;
        if (_wfopen_s(&f, (s + L"\\steamapps\\libraryfolders.vdf").c_str(), L"rb") == 0 && f) {
            char line[2048];
            while (fgets(line, sizeof(line), f)) {
                const char* p = strstr(line, "\"path\"");
                if (!p) continue;
                p = strchr(p + 6, '"');
                if (!p) continue;
                string v;
                for (++p; *p && *p != '"'; ++p) {
                    if (*p == '\\' && p[1] == '\\') ++p;
                    v += *p;
                }
                libs.push_back(Widen(v));
            }
            fclose(f);
        }
    }
    for (const wstring& l : libs) {
        wstring g = l + L"\\steamapps\\common\\Portal 2";
        if (FileExists(g + L"\\portal2.exe")) return g;
    }
    return {};
}

static wstring GameDir()
{
    if (!g_set.gameDir.empty() && FileExists(g_set.gameDir + L"\\portal2.exe")) return g_set.gameDir;
    static wstring found = FindPortal2();
    return found;
}

// ---------------------------------------------------------------------------------------------
// ffmpeg (advanced mode): chosen path, else the copy P2Render downloaded, else PATH.

static wstring FindFileUnder(const wstring& dir, const wchar_t* name, int depth)
{
    if (FileExists(dir + L"\\" + name)) return dir + L"\\" + name;
    if (depth == 0) return {};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    wstring r;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
            r = FindFileUnder(dir + L"\\" + fd.cFileName, name, depth - 1);
    } while (r.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return r;
}

static wstring FfmpegPath()
{
    if (!g_set.ffmpegPath.empty() && FileExists(g_set.ffmpegPath)) return g_set.ffmpegPath;
    wstring local = FindFileUnder(g_dataDir + L"\\ffmpeg", L"ffmpeg.exe", 3);
    if (!local.empty()) return local;
    wchar_t p[MAX_PATH];
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, p, nullptr)) return p;
    return {};
}

struct FfmpegDownload {
    bool running = false;
    int percent = 0;
    string error;
};
static FfmpegDownload g_ffdl;   // guarded by g_mx

class DownloadProgress : public IBindStatusCallback {
public:
    HRESULT STDMETHODCALLTYPE OnProgress(ULONG done, ULONG total, ULONG, LPCWSTR) override
    {
        if (total) {
            std::lock_guard<std::mutex> l(g_mx);
            g_ffdl.percent = (int)((double)done * 90 / total); // the last 10 % = extraction
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override
    {
        if (riid == IID_IUnknown || riid == IID_IBindStatusCallback) {
            *p = this;
            return S_OK;
        }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD, IBinding*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnLowResource(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD*, BINDINFO*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }
};

static bool RunAndWait(const wstring& cmd, DWORD timeoutMs)
{
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(0);
    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// Downloads the latest release build of ffmpeg (BtbN, win64, GPL, shared libraries ~85 MB) into
// %LOCALAPPDATA%\P2Render\ffmpeg and extracts it with Windows' tar.
static void DownloadFfmpeg()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto fail = [](const string& e) {
        std::lock_guard<std::mutex> l(g_mx);
        g_ffdl.running = false;
        g_ffdl.error = e;
        Log("ffmpeg download: %s", e.c_str());
    };
    wstring dir = g_dataDir + L"\\ffmpeg";
    wstring json = g_dataDir + L"\\ffmpeg_release.json", zip = g_dataDir + L"\\ffmpeg.zip";
    string url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip";
    if (SUCCEEDED(URLDownloadToFileW(nullptr, L"https://api.github.com/repos/BtbN/FFmpeg-Builds/releases/latest", json.c_str(), 0, nullptr))) {
        // Prefer the newest numbered release ("ffmpeg-nX.Y-latest-win64-gpl-shared-X.Y.zip").
        FILE* f = nullptr;
        string body;
        if (_wfopen_s(&f, json.c_str(), L"rb") == 0 && f) {
            char b[65536];
            size_t n;
            while ((n = fread(b, 1, sizeof(b), f)) > 0) body.append(b, n);
            fclose(f);
        }
        DeleteFileW(json.c_str());
        // The newest numbered release, but 8.x first: ffmpeg 9 needs NVENC API 13.1 (NVIDIA driver
        // 610+), 8.x works with older drivers.
        string best, best8;
        for (size_t p = body.find("\"browser_download_url\""); p != string::npos; p = body.find("\"browser_download_url\"", p + 1)) {
            size_t a = body.find('"', body.find(':', p) + 1), e = body.find('"', a + 1);
            string u = body.substr(a + 1, e - a - 1);
            if (u.find("/ffmpeg-n") != string::npos && u.find("-win64-gpl-shared-") != string::npos && u.size() > 4 &&
                u.compare(u.size() - 4, 4, ".zip") == 0) {
                if (u > best) best = u;
                if (u.find("/ffmpeg-n8.") != string::npos && u > best8) best8 = u;
            }
        }
        if (!best8.empty()) url = best8;
        else if (!best.empty()) url = best;
    }
    Log("ffmpeg download: %s", url.c_str());
    DownloadProgress progress;
    HRESULT hr = URLDownloadToFileW(nullptr, Widen(url).c_str(), zip.c_str(), 0, &progress);
    if (FAILED(hr)) return fail("téléchargement impossible (pas de connexion ?)");
    {
        std::lock_guard<std::mutex> l(g_mx);
        g_ffdl.percent = 92;
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    bool ok = RunAndWait(L"\"" + wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + dir + L"\"", 300000);
    DeleteFileW(zip.c_str());
    if (!ok || FindFileUnder(dir, L"ffmpeg.exe", 3).empty()) return fail("extraction de l'archive impossible");
    std::lock_guard<std::mutex> l(g_mx);
    g_ffdl.running = false;
    g_ffdl.percent = 100;
    g_ffdl.error.clear();
    Log("ffmpeg ready: %s", Narrow(FfmpegPath()).c_str());
}

// Mutes (or restores) the game in the Windows mixer: invisible rendering must not be heard, but the
// engine still mixes the sound that goes into the video. Windows remembers a per-app mute, so the
// game has to be unmuted before it goes away, otherwise a later normal launch stays silent.
static void MuteProcessAudio(DWORD pid, bool mute)
{
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
        IAudioSessionManager2* mgr = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr))) {
            IAudioSessionEnumerator* list = nullptr;
            if (SUCCEEDED(mgr->GetSessionEnumerator(&list))) {
                int n = 0;
                list->GetCount(&n);
                for (int i = 0; i < n; ++i) {
                    IAudioSessionControl* c = nullptr;
                    IAudioSessionControl2* c2 = nullptr;
                    DWORD sp = 0;
                    if (SUCCEEDED(list->GetSession(i, &c)) && SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c2))) &&
                        SUCCEEDED(c2->GetProcessId(&sp)) && sp == pid) {
                        ISimpleAudioVolume* vol = nullptr;
                        if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&vol)))) {
                            BOOL muted = FALSE;
                            vol->GetMute(&muted);
                            if (!!muted != mute) vol->SetMute(mute, nullptr);
                            vol->Release();
                        }
                    }
                    if (c2) c2->Release();
                    if (c) c->Release();
                }
                list->Release();
            }
            mgr->Release();
        }
        dev->Release();
    }
    en->Release();
}

// ---------------------------------------------------------------------------------------------
// Demo files

struct DemoInfo {
    bool ok = false;
    string map;
    float seconds = 0;
    int ticks = 0;
};

static DemoInfo ReadDemo(const wstring& path)
{
    DemoInfo d;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return d;
    unsigned char h[0x430];
    size_t n = fread(h, 1, sizeof(h), f);
    fclose(f);
    if (n < sizeof(h) || memcmp(h, "HL2DEMO", 8) != 0) return d;
    char map[261] = {};
    memcpy(map, h + 0x218, 260);
    d.map = map;
    memcpy(&d.seconds, h + 0x420, 4);
    memcpy(&d.ticks, h + 0x424, 4);
    d.ok = true;
    return d;
}

// ---------------------------------------------------------------------------------------------
// Queue

enum class ItemState { Pending, Loading, Recording, Encoding, Done, Failed, Cancelled };

static const char* StateName(ItemState s)
{
    switch (s) {
    case ItemState::Pending: return "pending";
    case ItemState::Loading: return "loading";
    case ItemState::Recording: return "recording";
    case ItemState::Encoding: return "encoding";
    case ItemState::Done: return "done";
    case ItemState::Failed: return "failed";
    default: return "cancelled";
    }
}

struct Item {
    int id;
    wstring path;
    DemoInfo info;
    ItemState state = ItemState::Pending;
    int frames = 0, expected = 0, encoded = 0;
    int renderMs = 0;
    string message;
    wstring output;
};

static std::vector<Item> g_items;
static int g_nextId = 1;
static bool g_running;
static std::atomic<bool> g_cancel;
static string g_status = "Prêt";      // one line under the queue
static string g_gameState = "stopped"; // stopped | starting | ready | busy
static DWORD g_gamePid;
static std::atomic<DWORD> g_lastPoll;

// Live "Visualiser" job (POC): one demo rendered hidden while ffmpeg writes a live stream (HLS /
// fragmented MP4) into a folder owned by the caller, which serves it to its player.
struct StreamSpec {
    bool active = false;
    int id = 0;              // item id this applies to
    int width = 1280, height = 720, fps = 60;
    wstring dir, out;        // output folder + name of the stream file (playlist or .mp4)
    wstring ffmpegPath;      // empty = auto
    wstring ffArgs;          // ffmpeg output options (encoders + muxer), see the POC
};
static StreamSpec g_stream;  // guarded by g_mx

static Item* FindItem(int id)
{
    for (Item& i : g_items)
        if (i.id == id) return &i;
    return nullptr;
}

static void AddDemo(const wstring& path)
{
    Item it;
    it.id = g_nextId++;
    it.path = path;
    it.info = ReadDemo(path);
    if (!it.info.ok) {
        it.state = ItemState::Failed;
        it.message = "ce fichier n'est pas une démo Source";
    }
    g_items.push_back(it);
}

// ---------------------------------------------------------------------------------------------
// Game session

static HANDLE g_gameProc;
static HANDLE g_ctrlMap;
static P2RControl* g_ctrl;
static int g_sessionW, g_sessionH;
static bool g_sessionHidden;

static bool GameAlive() { return g_gameProc && WaitForSingleObject(g_gameProc, 0) == WAIT_TIMEOUT; }

static void CloseSession()
{
    if (g_ctrl) UnmapViewOfFile(g_ctrl);
    if (g_ctrlMap) CloseHandle(g_ctrlMap);
    if (g_gameProc) CloseHandle(g_gameProc);
    g_ctrl = nullptr;
    g_ctrlMap = nullptr;
    g_gameProc = nullptr;
    g_gamePid = 0;
}

static bool OtherPortal2Running()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = {sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, L"portal2.exe") && pe.th32ProcessID != g_gamePid) found = true;
    CloseHandle(snap);
    return found;
}

static void SetStatus(const string& s)
{
    std::lock_guard<std::mutex> l(g_mx);
    g_status = s;
}

// Runs p2r_launch.exe and waits for the core to be ready. Called from the worker thread.
static bool StartGame(int w, int h, bool hidden, string& err)
{
    wstring game = GameDir();
    if (game.empty()) {
        err = "Portal 2 introuvable : indique son dossier dans les réglages";
        return false;
    }
    if (OtherPortal2Running()) {
        err = "Portal 2 est déjà ouvert : ferme-le, P2Render le lance lui-même";
        return false;
    }
    wstring dir = ExeDir();
    wstring cmd = L"\"" + dir + L"p2r_launch.exe\" \"" + game + L"\" \"" + dir + L"p2r_core.dll\" -game portal2 -novid -sw -noborder -w " +
                  std::to_wstring(w) + L" -h " + std::to_wstring(h);
    Log("launch: %s", Narrow(cmd).c_str());
    // Inherited by p2r_launch.exe and the game: start hidden, keep the window hidden (core).
    SetEnvironmentVariableW(L"P2R_HIDDEN", hidden ? L"1" : L"0");
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE rd, wr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(0);
    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        err = "p2r_launch.exe n'a pas pu démarrer";
        return false;
    }
    CloseHandle(wr);
    string out;
    char buf[512];
    DWORD n;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    Log("launch exit %lu: %s", code, out.c_str());
    DWORD pid = 0;
    if (code != 0 || sscanf_s(out.c_str(), "pid %lu", &pid) != 1) {
        err = "le lancement de Portal 2 a échoué : " + out;
        return false;
    }
    g_gamePid = pid;
    g_gameProc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
    g_sessionW = w;
    g_sessionH = h;
    g_sessionHidden = hidden;

    // Wait for the control block.
    wchar_t name[64];
    swprintf_s(name, L"Local\\p2render_ctrl_%lu", pid);
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 180000) {
        if (g_cancel) {
            err = "annulé";
            return false;
        }
        if (!GameAlive()) {
            err = "Portal 2 s'est fermé pendant le démarrage";
            return false;
        }
        if (hidden) MuteProcessAudio(pid, true);
        if (!g_ctrl) {
            g_ctrlMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
            if (g_ctrlMap) g_ctrl = (P2RControl*)MapViewOfFile(g_ctrlMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(P2RControl));
        }
        if (g_ctrl && g_ctrl->magic == P2R_CTRL_MAGIC) {
            if (g_ctrl->version != P2R_CTRL_VERSION) {
                err = "versions de P2Render incohérentes";
                return false;
            }
            if (g_ctrl->core_state == CORE_READY) return true;
            if (g_ctrl->core_state == CORE_BROKEN) {
                err = string("Portal 2 ne peut pas faire le rendu : ") + g_ctrl->core_message;
                return false;
            }
        }
        Sleep(100);
    }
    err = "Portal 2 n'a pas fini de démarrer à temps";
    return false;
}

static void QuitGame()
{
    if (g_ctrl && GameAlive()) {
        MuteProcessAudio(g_gamePid, false); // leave the game audible for its next, normal launch
        g_ctrl->cmd = CMD_QUIT;
        MemoryBarrier();
        ++g_ctrl->cmd_seq;
        if (WaitForSingleObject(g_gameProc, 15000) != WAIT_OBJECT_0) TerminateProcess(g_gameProc, 0);
    }
    CloseSession();
}

static int Bitrate(const Settings& s)
{
    double bpp = s.quality == "standard" ? 0.15 : s.quality == "max" ? 0.6 : 0.33;
    double kbps = (double)s.width * s.height * s.fps * bpp / 1000.0;
    if (s.codec == "hevc") kbps *= 0.7;
    return std::max(4000, (int)kbps);
}

static wstring UniqueOutput(const wstring& dir, const wstring& base, const wstring& ext)
{
    wstring p = dir + L"\\" + base + L"." + ext;
    for (int i = 2; FileExists(p); ++i) p = dir + L"\\" + base + L"_" + std::to_wstring(i) + L"." + ext;
    return p;
}

// Renders one item; returns false if the session is unusable afterwards.
static bool RenderItem(int id)
{
    Settings s;
    StreamSpec stream;
    wstring src;
    {
        std::lock_guard<std::mutex> l(g_mx);
        s = g_set;
        Item* it = FindItem(id);
        if (!it) return true;
        src = it->path;
        if (g_stream.active && g_stream.id == id) {
            stream = g_stream;
            s.width = stream.width;
            s.height = stream.height;
            s.fps = stream.fps;
            s.showWindow = false; // a live stream is always rendered hidden
        }
        it->state = ItemState::Loading;
        it->frames = it->encoded = it->renderMs = 0;
        it->expected = (int)(it->info.seconds * s.fps + 0.5f);
        it->message.clear();
    }
    auto finish = [&](ItemState st, const string& msg, const wstring& out = L"") {
        std::lock_guard<std::mutex> l(g_mx);
        if (Item* it = FindItem(id)) {
            it->state = st;
            it->message = msg;
            it->output = out;
        }
    };

    bool hidden = !s.showWindow;
    if (g_ctrl && GameAlive() && (g_sessionW != s.width || g_sessionH != s.height || g_sessionHidden != hidden))
        QuitGame(); // new resolution or display mode
    wstring ffmpeg;
    if (s.advanced || stream.active) {
        {
            std::lock_guard<std::mutex> l(g_mx);
            ffmpeg = stream.active && !stream.ffmpegPath.empty() ? stream.ffmpegPath : FfmpegPath();
        }
        if (ffmpeg.empty()) {
            finish(ItemState::Failed, stream.active ? "ffmpeg introuvable (le flux ne peut pas être écrit)"
                                                    : "mode avancé : ffmpeg introuvable (télécharge-le ou choisis ffmpeg.exe dans les réglages)");
            return true;
        }
    }
    if (!GameAlive()) {
        CloseSession();
        {
            std::lock_guard<std::mutex> l(g_mx);
            g_gameState = "starting";
            g_status = "Démarrage de Portal 2…";
        }
        string err;
        if (!StartGame(s.width, s.height, hidden, err)) {
            if (GameAlive()) {
                MuteProcessAudio(g_gamePid, false); // don't leave the game silent for its next launch
                TerminateProcess(g_gameProc, 1);
            }
            CloseSession();
            finish(g_cancel ? ItemState::Cancelled : ItemState::Failed, err);
            SetStatus(err);
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> l(g_mx);
        g_gameState = "busy";
        g_status = "Rendu en cours";
    }

    // The engine plays demos from the game folder: copy it there for the time of the render.
    wstring game = GameDir();
    wstring tmpDir = game + L"\\portal2\\p2render_tmp";
    CreateDirectoryW(tmpDir.c_str(), nullptr);
    wstring tmpDemo = tmpDir + L"\\demo" + std::to_wstring(id) + L".dem";
    if (!CopyFileW(src.c_str(), tmpDemo.c_str(), FALSE)) {
        finish(ItemState::Failed, "impossible de copier la démo dans le dossier du jeu");
        return true;
    }
    wstring out;
    if (stream.active) {
        SHCreateDirectoryExW(nullptr, stream.dir.c_str(), nullptr);
        out = stream.dir + L"\\" + stream.out;
    } else {
        CreateDirectoryW(s.outDir.c_str(), nullptr);
        SHCreateDirectoryExW(nullptr, s.outDir.c_str(), nullptr);
        out = UniqueOutput(s.outDir, StripExt(FileName(src)), s.advanced ? Widen(s.container) : L"mp4");
    }

    P2RJob job = {};
    sprintf_s(job.demo, "p2render_tmp/demo%d", id);
    wcscpy_s(job.out_path, out.c_str());
    job.fps = s.fps;
    job.codec = s.codec == "hevc" ? P2R_HEVC : P2R_H264;
    job.bitrate_kbps = Bitrate(s);
    job.show_window = s.showWindow;
    if (stream.active) {
        job.use_ffmpeg = 1;
        job.stream = 1;
        wcscpy_s(job.ffmpeg_path, ffmpeg.c_str());
        wcsncpy_s(job.ff_video_args, stream.ffArgs.c_str(), _TRUNCATE);
    } else if (s.advanced) {
        job.use_ffmpeg = 1;
        wcscpy_s(job.ffmpeg_path, ffmpeg.c_str());
        wcsncpy_s(job.ff_video_args, Widen(s.ffVideo).c_str(), _TRUNCATE);
        wcsncpy_s(job.ff_audio_args, Widen(s.ffAudio).c_str(), _TRUNCATE);
    }
    g_ctrl->job = job;
    g_ctrl->cmd = CMD_RENDER;
    MemoryBarrier();
    int seq = ++g_ctrl->cmd_seq;
    Log("job %d (seq %d): %s -> %s", id, seq, Narrow(src).c_str(), Narrow(out).c_str());

    bool cancelSent = false, sessionOk = true;
    DWORD lastMute = 0;
    for (;;) {
        Sleep(100);
        if (hidden && GetTickCount() - lastMute > 1000) {
            MuteProcessAudio(g_gamePid, true);
            lastMute = GetTickCount();
        }
        if (!GameAlive()) {
            finish(ItemState::Failed, "Portal 2 s'est fermé pendant le rendu");
            sessionOk = false;
            break;
        }
        if (g_cancel && !cancelSent) {
            g_ctrl->cmd = CMD_CANCEL;
            MemoryBarrier();
            ++g_ctrl->cmd_seq;
            cancelSent = true;
        }
        if (g_ctrl->ack_seq < seq || g_ctrl->job_id != seq) continue;
        int js = g_ctrl->job_state;
        {
            std::lock_guard<std::mutex> l(g_mx);
            if (Item* it = FindItem(id)) {
                it->frames = g_ctrl->frames;
                it->encoded = g_ctrl->frames_encoded;
                it->renderMs = g_ctrl->render_ms;
                it->message = g_ctrl->job_message;
                if (js == JOB_LOADING) it->state = ItemState::Loading;
                else if (js == JOB_RECORDING) it->state = ItemState::Recording;
                else if (js == JOB_ENCODING) it->state = ItemState::Encoding;
            }
        }
        if (js == JOB_DONE) {
            finish(ItemState::Done, "", out);
            break;
        }
        if (js == JOB_FAILED) {
            finish(cancelSent ? ItemState::Cancelled : ItemState::Failed, cancelSent ? "annulé" : g_ctrl->job_message);
            break;
        }
    }
    // The engine may still hold the demo for a moment.
    for (int i = 0; i < 20 && !DeleteFileW(tmpDemo.c_str()) && FileExists(tmpDemo); ++i) Sleep(250);
    RemoveDirectoryW(tmpDir.c_str());
    return sessionOk;
}

static void Worker()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // audio session muting
    for (;;) {
        int next = 0;
        {
            std::lock_guard<std::mutex> l(g_mx);
            if (!g_cancel)
                for (Item& i : g_items)
                    if (i.state == ItemState::Pending) {
                        next = i.id;
                        break;
                    }
        }
        if (!next) break;
        if (!RenderItem(next) && !GameAlive()) {
            // Starting the game failed: stop the queue rather than retrying for every demo.
            std::lock_guard<std::mutex> l(g_mx);
            if (!g_cancel) {
                Item* it = FindItem(next);
                if (it && it->state == ItemState::Failed && g_gamePid == 0) break;
            }
        }
    }
    bool close;
    {
        std::lock_guard<std::mutex> l(g_mx);
        close = g_set.closeGame;
    }
    if (close) QuitGame();
    std::lock_guard<std::mutex> l(g_mx);
    if (g_cancel) {
        g_status = "Rendu annulé";
    } else {
        int done = 0, failed = 0;
        for (Item& i : g_items) {
            if (i.state == ItemState::Done) ++done;
            if (i.state == ItemState::Failed) ++failed;
        }
        if (g_status.rfind("Portal 2", 0) != 0 && g_status.rfind("le lancement", 0) != 0)
            g_status = failed ? "Terminé avec des erreurs" : "Terminé";
    }
    g_gameState = GameAlive() ? "ready" : "stopped";
    g_running = false;
    g_cancel = false;
}

// ---------------------------------------------------------------------------------------------
// Native dialogs (run on their own STA thread)

static std::vector<wstring> PickFiles(bool folder, const wstring& title, bool exe = false)
{
    std::vector<wstring> result;
    std::thread t([&] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IFileOpenDialog* dlg = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
            DWORD opt = 0;
            dlg->GetOptions(&opt);
            if (folder) {
                opt |= FOS_PICKFOLDERS;
            } else if (exe) {
                opt |= FOS_FILEMUSTEXIST;
                COMDLG_FILTERSPEC spec[] = {{L"ffmpeg.exe", L"ffmpeg.exe"}, {L"Programmes", L"*.exe"}};
                dlg->SetFileTypes(2, spec);
            } else {
                opt |= FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST;
                COMDLG_FILTERSPEC spec[] = {{L"Démos Portal 2", L"*.dem"}, {L"Tous les fichiers", L"*.*"}};
                dlg->SetFileTypes(2, spec);
                wstring def = GameDir() + L"\\portal2";
                IShellItem* si = nullptr;
                if (FileExists(def) && SUCCEEDED(SHCreateItemFromParsingName(def.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
                    dlg->SetDefaultFolder(si);
                    si->Release();
                }
            }
            dlg->SetOptions(opt);
            dlg->SetTitle(title.c_str());
            HWND owner = GetForegroundWindow();
            if (SUCCEEDED(dlg->Show(owner))) {
                IShellItemArray* arr = nullptr;
                if (SUCCEEDED(dlg->GetResults(&arr))) {
                    DWORD n = 0;
                    arr->GetCount(&n);
                    for (DWORD i = 0; i < n; ++i) {
                        IShellItem* it = nullptr;
                        PWSTR p = nullptr;
                        if (SUCCEEDED(arr->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                            result.push_back(p);
                            CoTaskMemFree(p);
                        }
                        if (it) it->Release();
                    }
                    arr->Release();
                }
            }
            dlg->Release();
        }
        CoUninitialize();
    });
    t.join();
    return result;
}

// ---------------------------------------------------------------------------------------------
// HTTP

static string g_token;
static wstring g_uiDir;

struct Request {
    string method, path, query, body;
    std::vector<std::pair<string, string>> headers;
    string Header(const char* name) const
    {
        for (auto& h : headers)
            if (!_stricmp(h.first.c_str(), name)) return h.second;
        return {};
    }
};

static void Send(SOCKET s, int code, const char* type, const string& body)
{
    const char* text = code == 200 ? "OK" : code == 404 ? "Not Found" : code == 403 ? "Forbidden" : "Bad Request";
    char head[512];
    int n = sprintf_s(head, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                      code, text, type, body.size());
    send(s, head, n, 0);
    size_t off = 0;
    while (off < body.size()) {
        int r = send(s, body.data() + off, (int)std::min<size_t>(body.size() - off, 1 << 20), 0);
        if (r <= 0) break;
        off += r;
    }
}

static string StateJson()
{
    std::lock_guard<std::mutex> l(g_mx);
    const Settings& s = g_set;
    wstring game = GameDir();
    string j = "{";
    j += "\"game\":{\"path\":" + Q(game) + ",\"found\":" + (game.empty() ? "false" : "true") + ",\"state\":" + Q(g_gameState) + "},";
    j += "\"settings\":{\"width\":" + std::to_string(s.width) + ",\"height\":" + std::to_string(s.height) +
         ",\"fps\":" + std::to_string(s.fps) + ",\"codec\":" + Q(s.codec) + ",\"quality\":" + Q(s.quality) +
         ",\"outDir\":" + Q(s.outDir) + ",\"gameDir\":" + Q(s.gameDir) + ",\"showWindow\":" + (s.showWindow ? "true" : "false") +
         ",\"closeGame\":" + (s.closeGame ? "true" : "false") + ",\"bitrate\":" + std::to_string(Bitrate(s)) +
         ",\"advanced\":" + (s.advanced ? "true" : "false") + ",\"ffmpegPath\":" + Q(s.ffmpegPath) + ",\"container\":" + Q(s.container) +
         ",\"ffVideo\":" + Q(s.ffVideo) + ",\"ffAudio\":" + Q(s.ffAudio) + ",\"ffUi\":" + Q(s.ffUi) + "},";
    wstring ff = FfmpegPath();
    j += "\"ffmpeg\":{\"path\":" + Q(ff) + ",\"found\":" + (ff.empty() ? "false" : "true") + ",\"downloading\":" +
         (g_ffdl.running ? "true" : "false") + ",\"percent\":" + std::to_string(g_ffdl.percent) + ",\"error\":" + Q(g_ffdl.error) + "},";
    j += "\"running\":" + string(g_running ? "true" : "false") + ",\"status\":" + Q(g_status) + ",\"items\":[";
    bool first = true;
    for (const Item& i : g_items) {
        if (!first) j += ",";
        first = false;
        j += "{\"id\":" + std::to_string(i.id) + ",\"file\":" + Q(FileName(i.path)) + ",\"path\":" + Q(i.path) +
             ",\"map\":" + Q(i.info.map) + ",\"seconds\":" + std::to_string(i.info.seconds) + ",\"state\":" + Q(StateName(i.state)) +
             ",\"frames\":" + std::to_string(i.frames) + ",\"expected\":" + std::to_string(i.expected) +
             ",\"encoded\":" + std::to_string(i.encoded) + ",\"renderMs\":" + std::to_string(i.renderMs) +
             ",\"message\":" + Q(i.message) + ",\"output\":" + Q(i.output) + "}";
    }
    j += "]}";
    return j;
}

static string Handle(const Request& r, int& code, const char*& type)
{
    code = 200;
    type = "application/json; charset=utf-8";
    if (r.path.rfind("/api/", 0) != 0) {
        // Static interface files.
        string p = r.path == "/" ? "/index.html" : r.path;
        if (p.find("..") != string::npos) {
            code = 404;
            return "";
        }
        wstring file = g_uiDir + Widen(p);
        std::replace(file.begin(), file.end(), L'/', L'\\');
        FILE* f = nullptr;
        if (_wfopen_s(&f, file.c_str(), L"rb") || !f) {
            code = 404;
            return "";
        }
        string data;
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
        fclose(f);
        type = p.size() > 4 && p.substr(p.size() - 4) == ".svg" ? "image/svg+xml"
               : p.size() > 3 && p.substr(p.size() - 3) == ".js" ? "text/javascript; charset=utf-8"
               : p.size() > 4 && p.substr(p.size() - 4) == ".css" ? "text/css; charset=utf-8"
                                                                    : "text/html; charset=utf-8";
        return data;
    }
    if (r.Header("X-Token") != g_token) {
        code = 403;
        return "{\"error\":\"token\"}";
    }
    g_lastPoll = GetTickCount();
    const string& b = r.body;

    if (r.path == "/api/state") return StateJson();

    if (r.path == "/api/settings" && r.method == "POST") {
        std::lock_guard<std::mutex> l(g_mx);
        Settings& s = g_set;
        s.width = std::clamp(JsonInt(b, "width", s.width), 320, 7680);
        s.height = std::clamp(JsonInt(b, "height", s.height), 240, 4320);
        s.fps = std::clamp(JsonInt(b, "fps", s.fps), 1, 1000);
        s.codec = JsonStr(b, "codec", s.codec) == "hevc" ? "hevc" : "h264";
        string q = JsonStr(b, "quality", s.quality);
        s.quality = q == "standard" || q == "max" ? q : "high";
        string v;
        if (JsonRaw(b, "outDir", v) && !v.empty()) s.outDir = Widen(v);
        if (JsonRaw(b, "gameDir", v)) s.gameDir = Widen(v);
        s.showWindow = JsonBool(b, "showWindow", s.showWindow);
        s.closeGame = JsonBool(b, "closeGame", s.closeGame);
        s.advanced = JsonBool(b, "advanced", s.advanced);
        string c = JsonStr(b, "container", s.container);
        if (c == "mp4" || c == "mkv" || c == "mov") s.container = c;
        if (JsonRaw(b, "ffVideo", v)) s.ffVideo = v;
        if (JsonRaw(b, "ffAudio", v)) s.ffAudio = v;
        if (JsonRaw(b, "ffUi", v)) s.ffUi = v;
        SaveSettings();
        return "{\"ok\":true}";
    }
    if (r.path == "/api/add-dialog" && r.method == "POST") {
        auto files = PickFiles(false, L"Ajouter des démos");
        std::lock_guard<std::mutex> l(g_mx);
        for (auto& f : files) AddDemo(f);
        return "{\"added\":" + std::to_string(files.size()) + "}";
    }
    if (r.path == "/api/add-paths" && r.method == "POST") {
        // {"paths":["...","..."]}: paths dropped from Explorer (when the browser gives them).
        std::lock_guard<std::mutex> l(g_mx);
        size_t p = b.find('[');
        int added = 0;
        while (p != string::npos) {
            size_t a = b.find('"', p + 1);
            if (a == string::npos) break;
            size_t e = b.find('"', a + 1);
            if (e == string::npos) break;
            string path = b.substr(a + 1, e - a - 1);
            size_t k;
            while ((k = path.find("\\\\")) != string::npos) path.replace(k, 2, "\\");
            AddDemo(Widen(path));
            ++added;
            p = e;
        }
        return "{\"added\":" + std::to_string(added) + "}";
    }
    if (r.path == "/api/upload" && r.method == "POST") {
        // Raw body = the dropped .dem file; ?name=<file name>.
        string name = "demo.dem";
        size_t p = r.query.find("name=");
        if (p != string::npos) {
            name.clear();
            for (size_t i = p + 5; i < r.query.size() && r.query[i] != '&'; ++i) {
                if (r.query[i] == '%' && i + 2 < r.query.size()) {
                    name += (char)strtoul(r.query.substr(i + 1, 2).c_str(), nullptr, 16);
                    i += 2;
                } else name += r.query[i] == '+' ? ' ' : r.query[i];
            }
        }
        wstring wname = FileName(Widen(name));
        for (wchar_t& c : wname)
            if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
        wstring dir = g_dataDir + L"\\demos";
        CreateDirectoryW(dir.c_str(), nullptr);
        wstring path = dir + L"\\" + wname;
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") || !f) {
            code = 400;
            return "{\"error\":\"write\"}";
        }
        fwrite(b.data(), 1, b.size(), f);
        fclose(f);
        std::lock_guard<std::mutex> l(g_mx);
        AddDemo(path);
        return "{\"added\":1}";
    }
    if (r.path == "/api/remove" && r.method == "POST") {
        std::lock_guard<std::mutex> l(g_mx);
        int id = JsonInt(b, "id");
        g_items.erase(std::remove_if(g_items.begin(), g_items.end(), [&](const Item& i) {
                          return i.id == id && (i.state == ItemState::Pending || i.state == ItemState::Done ||
                                                i.state == ItemState::Failed || i.state == ItemState::Cancelled);
                      }),
                      g_items.end());
        return "{\"ok\":true}";
    }
    if (r.path == "/api/clear" && r.method == "POST") {
        std::lock_guard<std::mutex> l(g_mx);
        g_items.erase(std::remove_if(g_items.begin(), g_items.end(), [](const Item& i) {
                          return i.state == ItemState::Done || i.state == ItemState::Failed || i.state == ItemState::Cancelled;
                      }),
                      g_items.end());
        return "{\"ok\":true}";
    }
    if (r.path == "/api/retry" && r.method == "POST") {
        std::lock_guard<std::mutex> l(g_mx);
        if (Item* it = FindItem(JsonInt(b, "id")))
            if (it->state == ItemState::Failed || it->state == ItemState::Cancelled || it->state == ItemState::Done) {
                it->state = it->info.ok ? ItemState::Pending : ItemState::Failed;
                it->message.clear();
                it->frames = it->encoded = it->renderMs = 0;
            }
        return "{\"ok\":true}";
    }
    if (r.path == "/api/visualise" && r.method == "POST") {
        // Live "Visualiser" (POC): one demo, rendered hidden, streamed while it renders.
        // {"path","dir","out","ffArgs","ffmpegPath","width","height","fps"}
        StreamSpec sp;
        string v;
        wstring path;
        if (JsonRaw(b, "path", v)) path = Widen(v);
        if (JsonRaw(b, "dir", v)) sp.dir = Widen(v);
        if (JsonRaw(b, "out", v)) sp.out = Widen(v);
        if (JsonRaw(b, "ffArgs", v)) sp.ffArgs = Widen(v);
        if (JsonRaw(b, "ffmpegPath", v)) sp.ffmpegPath = Widen(v);
        sp.width = std::clamp(JsonInt(b, "width", sp.width), 320, 7680);
        sp.height = std::clamp(JsonInt(b, "height", sp.height), 240, 4320);
        sp.fps = std::clamp(JsonInt(b, "fps", sp.fps), 1, 1000);
        if (path.empty() || sp.dir.empty() || sp.out.empty() || sp.ffArgs.empty())
            return "{\"error\":\"path, dir, out et ffArgs sont obligatoires\"}";
        if (sp.out.find_first_of(L"\\\\/:*?\"<>|") != wstring::npos) return "{\"error\":\"out doit être un simple nom de fichier\"}";
        if (!FileExists(path)) return "{\"error\":\"la démo n'existe pas\"}";
        if (!ReadDemo(path).ok) return "{\"error\":\"ce fichier n'est pas une démo Source\"}";
        std::lock_guard<std::mutex> l(g_mx);
        if (g_running) return "{\"error\":\"un rendu est déjà en cours\"}";
        g_items.clear();
        AddDemo(path);
        sp.active = true;
        sp.id = g_items.back().id;
        g_stream = sp;
        g_running = true;
        g_cancel = false;
        g_status = "Rendu en cours";
        Log("visualise %d: %s -> %s\\%s", sp.id, Narrow(path).c_str(), Narrow(sp.dir).c_str(), Narrow(sp.out).c_str());
        std::thread(Worker).detach();
        return "{\"ok\":true,\"id\":" + std::to_string(sp.id) + "}";
    }
    if (r.path == "/api/start" && r.method == "POST") {
        std::lock_guard<std::mutex> l(g_mx);
        if (!g_running) {
            for (Item& i : g_items)
                if (i.state == ItemState::Cancelled) i.state = ItemState::Pending;
            g_running = true;
            g_cancel = false;
            g_status = "Rendu en cours";
            std::thread(Worker).detach();
        }
        return "{\"ok\":true}";
    }
    if (r.path == "/api/cancel" && r.method == "POST") {
        g_cancel = true;
        SetStatus("Annulation…");
        return "{\"ok\":true}";
    }
    if (r.path == "/api/quit-game" && r.method == "POST") {
        // Also restores the game's sound (see MuteProcessAudio) so a later normal launch hears it.
        std::lock_guard<std::mutex> l(g_mx);
        QuitGame();
        return "{\"ok\":true}";
    }
    if (r.path == "/api/browse-out" && r.method == "POST") {
        auto f = PickFiles(true, L"Dossier des vidéos");
        if (!f.empty()) {
            std::lock_guard<std::mutex> l(g_mx);
            g_set.outDir = f[0];
            SaveSettings();
        }
        return "{\"ok\":true}";
    }
    if (r.path == "/api/browse-game" && r.method == "POST") {
        auto f = PickFiles(true, L"Dossier de Portal 2 (celui qui contient portal2.exe)");
        if (!f.empty()) {
            std::lock_guard<std::mutex> l(g_mx);
            if (FileExists(f[0] + L"\\portal2.exe")) {
                g_set.gameDir = f[0];
                SaveSettings();
            } else {
                return "{\"error\":\"portal2.exe n'est pas dans ce dossier\"}";
            }
        }
        return "{\"ok\":true}";
    }
    if (r.path == "/api/ffmpeg-download" && r.method == "POST") {
        std::lock_guard<std::mutex> l(g_mx);
        if (!g_ffdl.running) {
            g_ffdl = FfmpegDownload();
            g_ffdl.running = true;
            std::thread(DownloadFfmpeg).detach();
        }
        return "{\"ok\":true}";
    }
    if (r.path == "/api/browse-ffmpeg" && r.method == "POST") {
        auto f = PickFiles(false, L"Choisir ffmpeg.exe", true);
        if (!f.empty()) {
            std::lock_guard<std::mutex> l(g_mx);
            if (_wcsicmp(FileName(f[0]).c_str(), L"ffmpeg.exe") != 0) return "{\"error\":\"choisis le fichier ffmpeg.exe\"}";
            g_set.ffmpegPath = f[0];
            SaveSettings();
        }
        return "{\"ok\":true}";
    }
    if (r.path == "/api/open" && r.method == "POST") {
        wstring target;
        bool folder = JsonBool(b, "folder");
        {
            std::lock_guard<std::mutex> l(g_mx);
            int id = JsonInt(b, "id");
            if (Item* it = FindItem(id)) target = it->output;
            if (id == 0) target = g_set.outDir;
        }
        if (target.empty()) return "{\"ok\":false}";
        if (folder && FileExists(target) && !(GetFileAttributesW(target.c_str()) & FILE_ATTRIBUTE_DIRECTORY)) {
            wstring args = L"/select,\"" + target + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            CreateDirectoryW(target.c_str(), nullptr);
            ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return "{\"ok\":true}";
    }
    code = 404;
    return "{\"error\":\"not found\"}";
}

static void ServeClient(SOCKET c)
{
    string data;
    char buf[65536];
    size_t headerEnd = string::npos;
    size_t contentLength = 0;
    for (;;) {
        int n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        data.append(buf, n);
        if (headerEnd == string::npos) {
            headerEnd = data.find("\r\n\r\n");
            if (headerEnd != string::npos) {
                string h = data.substr(0, headerEnd);
                std::transform(h.begin(), h.end(), h.begin(), ::tolower);
                size_t p = h.find("content-length:");
                if (p != string::npos) contentLength = strtoull(h.c_str() + p + 15, nullptr, 10);
            }
        }
        if (headerEnd != string::npos && data.size() >= headerEnd + 4 + contentLength) break;
        if (data.size() > (512u << 20)) break;
    }
    if (headerEnd == string::npos) {
        closesocket(c);
        return;
    }
    Request r;
    size_t le = data.find("\r\n");
    string line = data.substr(0, le);
    size_t s1 = line.find(' '), s2 = line.find(' ', s1 + 1);
    r.method = line.substr(0, s1);
    string target = line.substr(s1 + 1, s2 - s1 - 1);
    size_t qm = target.find('?');
    r.path = target.substr(0, qm);
    if (qm != string::npos) r.query = target.substr(qm + 1);
    size_t pos = le + 2;
    while (pos < headerEnd) {
        size_t e = data.find("\r\n", pos);
        if (e == string::npos || e > headerEnd) e = headerEnd;
        string h = data.substr(pos, e - pos);
        size_t colon = h.find(':');
        if (colon != string::npos) {
            string v = h.substr(colon + 1);
            while (!v.empty() && v[0] == ' ') v.erase(0, 1);
            r.headers.push_back({h.substr(0, colon), v});
        }
        pos = e + 2;
    }
    r.body = data.substr(headerEnd + 4, contentLength);
    int code;
    const char* type;
    string resp = Handle(r, code, type);
    Send(c, code, type, resp);
    shutdown(c, SD_SEND);
    closesocket(c);
}

// ---------------------------------------------------------------------------------------------

static string RandomToken()
{
    unsigned char b[16];
    BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    string t;
    char h[3];
    for (unsigned char c : b) {
        sprintf_s(h, "%02x", c);
        t += h;
    }
    return t;
}

static wstring FindEdge()
{
    const wchar_t* cands[] = {L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
                              L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe"};
    for (auto c : cands)
        if (FileExists(c)) return c;
    wchar_t p[MAX_PATH];
    DWORD sz = sizeof(p);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe", nullptr, RRF_RT_REG_SZ, nullptr, p, &sz) == ERROR_SUCCESS)
        return p;
    return {};
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int)
{
    // One instance at a time (a headless run exits silently: no invisible dialog left to click).
    bool headless = cmdLine && wcsstr(cmdLine, L"--no-window");
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\P2Render_single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!headless) MessageBoxW(nullptr, L"P2Render est déjà ouvert.", L"P2Render", MB_ICONINFORMATION);
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    g_dataDir = KnownFolder(FOLDERID_LocalAppData) + L"\\P2Render";
    CreateDirectoryW(g_dataDir.c_str(), nullptr);
    g_log = _wfsopen((ExeDir() + L"p2render.log").c_str(), L"w", _SH_DENYNO);
    g_uiDir = ExeDir() + L"ui";
    LoadSettings();
    g_token = RandomToken();

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(ls, (sockaddr*)&a, sizeof(a)) || listen(ls, 16)) {
        MessageBoxW(nullptr, L"Impossible d'ouvrir le port local de l'interface.", L"P2Render", MB_ICONERROR);
        return 1;
    }
    int alen = sizeof(a);
    getsockname(ls, (sockaddr*)&a, &alen);
    int port = ntohs(a.sin_port);
    string url = "http://127.0.0.1:" + std::to_string(port) + "/?t=" + g_token;
    Log("ui at %s", url.c_str());
    std::thread([ls] {
        for (;;) {
            SOCKET c = accept(ls, nullptr, nullptr);
            if (c == INVALID_SOCKET) break;
            std::thread(ServeClient, c).detach();
        }
    }).detach();

    // --no-window: server only (tests); the URL is in p2render.log.
    HANDLE edge = nullptr;
    if (!headless) {
        wstring exe = FindEdge();
        if (!exe.empty()) {
            wstring args = L"\"" + exe + L"\" --app=" + Widen(url) + L" --user-data-dir=\"" + g_dataDir +
                           L"\\webview\" --window-size=1240,820 --no-first-run --no-default-browser-check --disable-features=Translate" +
                           L" --disable-background-timer-throttling --disable-renderer-backgrounding --disable-backgrounding-occluded-windows";
            STARTUPINFOW si = {sizeof(si)};
            PROCESS_INFORMATION pi = {};
            std::vector<wchar_t> cl(args.begin(), args.end());
            cl.push_back(0);
            if (CreateProcessW(exe.c_str(), cl.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                edge = pi.hProcess;
            }
        }
        if (!edge) ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // Lifetime: until the Edge window closes, or the page stops polling (closed tab, other browser):
    // 20 s without a poll while idle (10 min headless). Never while a render runs.
    g_lastPoll = GetTickCount();
    for (;;) {
        if (edge && WaitForSingleObject(edge, 500) == WAIT_OBJECT_0 && !g_running) break;
        if (!edge) Sleep(500);
        DWORD idle = GetTickCount() - g_lastPoll;
        if (!g_running && idle > (headless ? 600000u : 20000u)) break;
    }
    Log("window closed");
    if (g_running) {
        g_cancel = true;
        for (int i = 0; i < 100 && g_running; ++i) Sleep(100);
    }
    if (GameAlive()) QuitGame();
    closesocket(ls);
    (void)single;
    return 0;
}
