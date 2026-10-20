#include "engine.h"

#include <windows.h>
#include <psapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

namespace engine {

// ---- layouts and slots (re/notes.md, "Engine interfaces") ----------------------------------

typedef void* (*CreateInterfaceFn)(const char* name, int* ret);

struct ConCommandBase {
    void* vtable;              // +0x00
    ConCommandBase* m_pNext;   // +0x04
    bool m_bRegistered;        // +0x08
    const char* m_pszName;     // +0x0C
    const char* m_pszHelpString; // +0x10
    int m_nFlags;              // +0x14
};
struct ConCommand : ConCommandBase {
    CommandCallback m_fnCommandCallback; // +0x18
    void* m_fnCompletionCallback;        // +0x1C
    uint8_t m_bits;                      // +0x20: bit1 = new-style callback
};
struct ConVar : ConCommandBase {
    void* iconvar_vtable;      // +0x18
    ConVar* m_pParent;         // +0x1C
    const char* m_pszDefaultValue; // +0x20
    char* m_pszString;         // +0x24
};
static_assert(sizeof(ConCommandBase) == 0x18, "ConCommandBase layout");

enum { FCVAR_DONTRECORD = 1 << 17 };
enum { ICVAR_RegisterConCommand = 9, ICVAR_FindCommandBase = 13, ICVAR_FindVar = 15 };
enum { ENG_IsPlayingDemo = 78, ENG_ClientCmd_Unrestricted = 108 };
enum { CLIENT_FrameStageNotify = 35 };
enum { VIDEOMODE_WriteMovieFrame = 23 };

template <class T> static T VFunc(void* obj, int i) { return (T)((*(void***)obj)[i]); }

// ---- state ----------------------------------------------------------------------------------

static void* g_cvar;
static void* g_engclient;
static void* g_cmdVtable;
static ConCommand g_cmds[16];
static int g_numCmds;

static void** g_videomode;     // engine global `videomode` (IVideoMode*)
static void* o_WriteMovieFrame;
static MovieFrameCallback g_movieCb;
static FrameCallback g_frameCb, g_initCb;

typedef void(__fastcall* FrameStageFn)(void*, void*, int);
static FrameStageFn o_FrameStageNotify;
static bool g_initDone;

typedef void(__cdecl* MsgFn)(const char*, ...);
static MsgFn g_ConMsg, g_Warning;
static char g_gameDir[MAX_PATH];

static void* Interface(const char* module, const char* name)
{
    HMODULE m = GetModuleHandleA(module);
    auto f = m ? (CreateInterfaceFn)GetProcAddress(m, "CreateInterface") : nullptr;
    return f ? f(name, nullptr) : nullptr;
}

static bool PatchSlot(void* obj, int slot, void* fn, void** orig)
{
    void** vt = *(void***)obj;
    if (vt[slot] == fn) return true;
    DWORD prot;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &prot)) return false;
    *orig = vt[slot];
    vt[slot] = fn;
    VirtualProtect(&vt[slot], sizeof(void*), prot, &prot);
    return true;
}

// Byte pattern with ?? wildcards, searched in the module's code. Returns the only match (null if
// none, or if several: an ambiguous pattern must not be trusted).
static uint8_t* FindPattern(const char* module, const char* pattern)
{
    int bytes[128], n = 0;
    for (const char* p = pattern; *p && n < 128;) {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?') { bytes[n++] = -1; p += 2; continue; }
        bytes[n++] = (int)strtoul(p, nullptr, 16);
        p += 2;
    }
    HMODULE m = GetModuleHandleA(module);
    MODULEINFO mi = {};
    if (!m || !GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi))) return nullptr;
    uint8_t* base = (uint8_t*)m;
    uint8_t* found = nullptr;
    int count = 0;
    for (size_t i = 0; i + n <= mi.SizeOfImage; ++i) {
        int j = 0;
        while (j < n && (bytes[j] < 0 || base[i + j] == bytes[j])) ++j;
        if (j == n) {
            if (!found) found = base + i;
            ++count;
        }
    }
    if (count != 1) LOG("pattern in %s: %d matches (%s)", module, count, pattern);
    return count == 1 ? found : nullptr;
}

// ---- movie frames ---------------------------------------------------------------------------

static void __fastcall Hook_WriteMovieFrame(void* self, void* edx, void* movieInfo)
{
    if (g_movieCb && g_movieCb()) return;
    ((void(__fastcall*)(void*, void*, void*))o_WriteMovieFrame)(self, edx, movieInfo);
}

static void InitMovieHook()
{
    // CL_TakeSnapshotAndSwap: `if (movie recording && !console && ...) {
    //   videomode->WriteMovieFrame(&cl_movieinfo); ++cl_movieinfo.frame; }` (engine!0x10097e82):
    // mov ecx,[videomode]; mov edx,[ecx]; mov eax,[edx+5Ch]; push offset cl_movieinfo; call eax;
    // inc dword ptr [...]
    uint8_t* p = FindPattern("engine.dll", "8B 0D ?? ?? ?? ?? 8B 11 8B 42 5C 68 ?? ?? ?? ?? FF D0 FF 05");
    if (!p) {
        LOG("WriteMovieFrame call site not found");
        return;
    }
    g_videomode = *(void***)(p + 2);
    if (!*g_videomode) {
        LOG("videomode is null");
        g_videomode = nullptr;
        return;
    }
    if (!PatchSlot(*g_videomode, VIDEOMODE_WriteMovieFrame, (void*)&Hook_WriteMovieFrame, &o_WriteMovieFrame)) {
        LOG("cannot patch CVideoMode vtable");
        g_videomode = nullptr;
        return;
    }
    LOG("WriteMovieFrame hooked (videomode %p)", *g_videomode);
}

void SetMovieFrameCallback(MovieFrameCallback cb) { g_movieCb = cb; }
bool MovieHookReady() { return o_WriteMovieFrame != nullptr; }

// ---- console ---------------------------------------------------------------------------------

static bool InitConsole()
{
    g_cvar = Interface("vstdlib.dll", "VEngineCvar007");
    g_engclient = Interface("engine.dll", "VEngineClient015");
    HMODULE t0 = GetModuleHandleA("tier0.dll");
    g_ConMsg = (MsgFn)GetProcAddress(t0, "ConMsg");
    g_Warning = (MsgFn)GetProcAddress(t0, "Warning");
    if (!g_cvar || !g_engclient) {
        LOG("engine interfaces missing: cvar %p engineclient %p", g_cvar, g_engclient);
        return false;
    }
    // Our commands reuse the vtable of an engine command; layout check on the way.
    auto find = VFunc<ConCommandBase*(__thiscall*)(void*, const char*)>(g_cvar, ICVAR_FindCommandBase);
    ConCommandBase* echo = find(g_cvar, "echo");
    if (!echo || !echo->m_pszName || strcmp(echo->m_pszName, "echo") != 0) {
        LOG("ICvar layout check failed");
        return false;
    }
    g_cmdVtable = echo->vtable;
    return true;
}

void AddCommand(const char* name, const char* help, CommandCallback cb)
{
    if (!g_cmdVtable || g_numCmds >= (int)(sizeof(g_cmds) / sizeof(g_cmds[0]))) return;
    ConCommand* c = &g_cmds[g_numCmds++];
    memset(c, 0, sizeof(*c));
    c->vtable = g_cmdVtable;
    c->m_pszName = name;
    c->m_pszHelpString = help;
    c->m_nFlags = FCVAR_DONTRECORD;
    c->m_fnCommandCallback = cb;
    c->m_bits = 0x02;
    VFunc<void(__thiscall*)(void*, ConCommandBase*)>(g_cvar, ICVAR_RegisterConCommand)(g_cvar, c);
}

static void VMsg(MsgFn fn, const char* prefix, const char* fmt, va_list ap)
{
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    LOG("%s%s", prefix, buf);
    if (fn) fn("[p2r] %s\n", buf);
}

void Msg(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    VMsg(g_ConMsg, "", fmt, ap);
    va_end(ap);
}

void Warn(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    VMsg(g_Warning, "warning: ", fmt, ap);
    va_end(ap);
}

void Run(const char* cmd)
{
    LOG("run: %s", cmd);
    if (g_engclient) VFunc<void(__thiscall*)(void*, const char*)>(g_engclient, ENG_ClientCmd_Unrestricted)(g_engclient, cmd);
}

const char* GetCvarString(const char* name)
{
    if (!g_cvar) return nullptr;
    ConVar* v = VFunc<ConVar*(__thiscall*)(void*, const char*)>(g_cvar, ICVAR_FindVar)(g_cvar, name);
    if (!v) return nullptr;
    if (v->m_pParent) v = v->m_pParent;
    return v->m_pszString;
}

bool IsPlayingDemo()
{
    return g_engclient && VFunc<bool(__thiscall*)(void*)>(g_engclient, ENG_IsPlayingDemo)(g_engclient);
}

const char* GameDir() { return g_gameDir; }

// ---- main-thread frame hook -------------------------------------------------------------------

void SetFrameCallback(FrameCallback cb) { g_frameCb = cb; }
void SetInitCallback(FrameCallback cb) { g_initCb = cb; }

static void __fastcall Hook_FrameStageNotify(void* self, void* edx, int stage)
{
    if (stage == 0) {
        if (!g_initDone) {
            g_initDone = true;
            if (InitConsole()) {
                InitMovieHook();
                if (g_initCb) g_initCb();
            }
        }
        if (g_frameCb && g_cmdVtable) g_frameCb();
    }
    o_FrameStageNotify(self, edx, stage);
}

bool TryHookFrame()
{
    if (!GetModuleHandleA("client.dll")) return false;
    void* client = Interface("client.dll", "VClient016");
    if (!client) return false;
    if (!g_gameDir[0]) {
        DWORD n = GetModuleFileNameA(nullptr, g_gameDir, MAX_PATH);
        while (n > 0 && g_gameDir[n - 1] != '\\') --n;
        strcpy_s(g_gameDir + n, MAX_PATH - n, "portal2");
    }
    bool ok = PatchSlot(client, CLIENT_FrameStageNotify, (void*)&Hook_FrameStageNotify, (void**)&o_FrameStageNotify);
    LOG("FrameStageNotify hook: %s", ok ? "ok" : "FAILED");
    return true; // do not retry either way
}

} // namespace engine
