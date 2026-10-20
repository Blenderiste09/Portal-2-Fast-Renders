// d3d9.dll proxy: dropped next to portal2.exe, it is found before the system d3d9.dll when
// shaderapidx9.dll resolves its imports (Direct3DCreate9, D3DPERF_SetOptions). Every export
// forwards to the real System32 d3d9.dll; Direct3DCreate9 is turned into Direct3DCreate9Ex
// (see d3d9ex.cpp).
#include <windows.h>
#include <d3d9.h>
#include <intrin.h>

#include "d3d9ex.h"
#include "log.h"

static HMODULE g_real;

static FARPROC Real(const char* name)
{
    FARPROC p = g_real ? GetProcAddress(g_real, name) : nullptr;
    if (!p) LOG("missing export in system d3d9.dll: %s", name);
    return p;
}

static bool LoadReal()
{
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 12) return false;
    strcat_s(path, "\\d3d9.dll");
    g_real = LoadLibraryA(path);
    LOG("system d3d9: %s -> %p", path, g_real);
    return g_real != nullptr;
}

// Exports with a known signature.

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdk)
{
    using Fn = IDirect3D9*(WINAPI*)(UINT);
    using FnEx = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    static Fn real = (Fn)Real("Direct3DCreate9");
    static FnEx realEx = (FnEx)Real("Direct3DCreate9Ex");
    void* caller = _ReturnAddress();
    if (d3d9ex::Enabled() && realEx) {
        IDirect3D9Ex* ex = nullptr;
        HRESULT hr = realEx(sdk, &ex);
        LOG("Direct3DCreate9(%u) from %p -> Direct3DCreate9Ex hr=0x%08lx obj=%p", sdk, caller, hr, ex);
        if (SUCCEEDED(hr) && ex) {
            d3d9ex::HookD3D9Ex(ex);
            return ex; // IDirect3D9Ex derives from IDirect3D9
        }
        LOG("Direct3DCreate9Ex failed, falling back to plain D3D9");
    }
    IDirect3D9* d3d = real ? real(sdk) : nullptr;
    LOG("Direct3DCreate9(%u) from %p -> plain %p", sdk, caller, d3d);
    return d3d;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out)
{
    using Fn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    static Fn real = (Fn)Real("Direct3DCreate9Ex");
    return real ? real(sdk, out) : E_NOTIMPL;
}

extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR name)
{
    using Fn = int(WINAPI*)(D3DCOLOR, LPCWSTR);
    static Fn real = (Fn)Real("D3DPERF_BeginEvent");
    return real ? real(col, name) : 0;
}

extern "C" int WINAPI D3DPERF_EndEvent()
{
    using Fn = int(WINAPI*)();
    static Fn real = (Fn)Real("D3DPERF_EndEvent");
    return real ? real() : 0;
}

extern "C" void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR name)
{
    using Fn = void(WINAPI*)(D3DCOLOR, LPCWSTR);
    static Fn real = (Fn)Real("D3DPERF_SetMarker");
    if (real) real(col, name);
}

extern "C" void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR name)
{
    using Fn = void(WINAPI*)(D3DCOLOR, LPCWSTR);
    static Fn real = (Fn)Real("D3DPERF_SetRegion");
    if (real) real(col, name);
}

extern "C" BOOL WINAPI D3DPERF_QueryRepeatFrame()
{
    using Fn = BOOL(WINAPI*)();
    static Fn real = (Fn)Real("D3DPERF_QueryRepeatFrame");
    return real ? real() : FALSE;
}

extern "C" void WINAPI D3DPERF_SetOptions(DWORD options)
{
    using Fn = void(WINAPI*)(DWORD);
    static Fn real = (Fn)Real("D3DPERF_SetOptions");
    if (real) real(options);
}

extern "C" DWORD WINAPI D3DPERF_GetStatus()
{
    using Fn = DWORD(WINAPI*)();
    static Fn real = (Fn)Real("D3DPERF_GetStatus");
    return real ? real() : 0;
}

// Rarely used exports with undocumented signatures: a jump to the real function keeps the
// caller's stack untouched.
#define FORWARD(name)                                                       \
    static FARPROC g_##name;                                                \
    extern "C" __declspec(naked) void Fwd_##name()                          \
    {                                                                       \
        __asm { jmp dword ptr [g_##name] }                                  \
    }
FORWARD(Direct3DShaderValidatorCreate9)
FORWARD(PSGPError)
FORWARD(PSGPSampleTexture)
FORWARD(DebugSetLevel)
FORWARD(DebugSetMute)
FORWARD(Direct3D9EnableMaximizedWindowedModeShim)
FORWARD(Direct3DCreate9On12)
FORWARD(Direct3DCreate9On12Ex)

static void ResolveForwards()
{
    static const struct { FARPROC* slot; const char* name; } table[] = {
        {&g_Direct3DShaderValidatorCreate9, "Direct3DShaderValidatorCreate9"},
        {&g_PSGPError, "PSGPError"},
        {&g_PSGPSampleTexture, "PSGPSampleTexture"},
        {&g_DebugSetLevel, "DebugSetLevel"},
        {&g_DebugSetMute, "DebugSetMute"},
        {&g_Direct3D9EnableMaximizedWindowedModeShim, "Direct3D9EnableMaximizedWindowedModeShim"},
        {&g_Direct3DCreate9On12, "Direct3DCreate9On12"},
        {&g_Direct3DCreate9On12Ex, "Direct3DCreate9On12Ex"},
    };
    for (auto& e : table) *e.slot = GetProcAddress(g_real, e.name);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        xlog::Init();
        LOG("d3d9ex proxy loaded (%s)", GetCommandLineA());
        if (!LoadReal()) {
            LOG("cannot load the system d3d9.dll");
            return FALSE;
        }
        ResolveForwards();
        d3d9ex::Init();
    }
    return TRUE;
}
