// p2r_core.dll: loaded into portal2.exe by p2r_launch.exe before the game's main() runs (nothing is
// installed in the game folder). When shaderapidx9.dll gets loaded, its import of Direct3DCreate9 is
// redirected to a function returning an IDirect3D9Ex (Direct3DCreate9Ex); d3d9ex.cpp then turns
// the device into a D3D9Ex device and capture.cpp renders the jobs P2Render sends.
#include <windows.h>
#include <d3d9.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "capture.h"
#include "d3d9ex.h"
#include "log.h"

// ---- Direct3DCreate9 -> Direct3DCreate9Ex ---------------------------------------------------

using Create9_t = IDirect3D9*(WINAPI*)(UINT);
using Create9Ex_t = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
static Create9_t g_origCreate9;   // the real d3d9!Direct3DCreate9
static Create9Ex_t g_create9Ex;

static IDirect3D9* WINAPI Hook_Direct3DCreate9(UINT sdk)
{
    void* caller = _ReturnAddress();
    if (d3d9ex::Enabled() && g_create9Ex) {
        IDirect3D9Ex* ex = nullptr;
        HRESULT hr = g_create9Ex(sdk, &ex);
        LOG("Direct3DCreate9(%u) from %p -> Direct3DCreate9Ex hr=0x%08lx obj=%p", sdk, caller, hr, ex);
        if (SUCCEEDED(hr) && ex) {
            d3d9ex::HookD3D9Ex(ex);
            return ex; // IDirect3D9Ex derives from IDirect3D9
        }
    }
    IDirect3D9* d3d = g_origCreate9(sdk);
    LOG("Direct3DCreate9(%u) from %p -> plain %p", sdk, caller, d3d);
    return d3d;
}

// shaderapidx9.dll calls d3d9!Direct3DCreate9 through its import table: we replace that IAT entry
// (as the old d3d9.dll proxy did). Patching d3d9 itself would put the Steam overlay, which hooks
// Direct3DCreate9 / CreateDevice in d3d9.dll, in the creation path of the Ex device, and the game
// then crashed right after its first Present.
static bool PatchImport(HMODULE mod, const char* dll, const char* func, void* hook)
{
    uint8_t* base = (uint8_t*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((char*)byName->Name, func) != 0) continue;
            DWORD prot;
            VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &prot);
            iat->u1.Function = (uintptr_t)hook;
            VirtualProtect(&iat->u1.Function, sizeof(void*), prot, &prot);
            return true;
        }
    }
    return false;
}

static void OnD3D9Loaded(HMODULE d3d9)
{
    if (g_origCreate9) return;
    g_origCreate9 = (Create9_t)GetProcAddress(d3d9, "Direct3DCreate9");
    g_create9Ex = (Create9Ex_t)GetProcAddress(d3d9, "Direct3DCreate9Ex");
    LOG("d3d9.dll at %p: Direct3DCreate9 %p, Direct3DCreate9Ex %p", d3d9, g_origCreate9, g_create9Ex);
}

// The loader announces shaderapidx9.dll before binding its imports: its entry point (run after the
// binding) is redirected once to patch the IAT, then restored.
static uint8_t* g_saEntry;
static uint8_t g_saEntryBytes[5];
static HMODULE g_shaderapi;

static BOOL WINAPI ShaderApiEntry(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    DWORD prot;
    VirtualProtect(g_saEntry, 5, PAGE_EXECUTE_READWRITE, &prot);
    memcpy(g_saEntry, g_saEntryBytes, 5);
    VirtualProtect(g_saEntry, 5, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), g_saEntry, 5);
    if (!g_origCreate9) OnD3D9Loaded(GetModuleHandleW(L"d3d9.dll"));
    bool ok = PatchImport(g_shaderapi, "d3d9.dll", "Direct3DCreate9", (void*)&Hook_Direct3DCreate9);
    LOG("shaderapidx9 import Direct3DCreate9: %s", ok ? "redirected" : "NOT FOUND (the game stays plain D3D9)");
    return ((BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID))g_saEntry)(inst, reason, reserved);
}

static void OnShaderApiLoaded(HMODULE mod)
{
    uint8_t* base = (uint8_t*)mod;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    g_shaderapi = mod;
    g_saEntry = base + nt->OptionalHeader.AddressOfEntryPoint;
    memcpy(g_saEntryBytes, g_saEntry, 5);
    DWORD prot;
    VirtualProtect(g_saEntry, 5, PAGE_EXECUTE_READWRITE, &prot);
    g_saEntry[0] = 0xE9;
    *(int32_t*)(g_saEntry + 1) = (int32_t)((uint8_t*)&ShaderApiEntry - (g_saEntry + 5));
    VirtualProtect(g_saEntry, 5, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), g_saEntry, 5);
    LOG("shaderapidx9.dll mapped at %p, entry %p redirected", mod, g_saEntry);
}

// ---- DLL load notification (ntdll) ------------------------------------------------------------

struct UNICODE_STR {
    USHORT Length, MaximumLength;
    PWSTR Buffer;
};
struct LDR_DLL_NOTIFICATION_DATA {
    ULONG Flags;
    const UNICODE_STR* FullDllName;
    const UNICODE_STR* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};
using LdrRegisterDllNotification_t = LONG(NTAPI*)(ULONG, void(CALLBACK*)(ULONG, const LDR_DLL_NOTIFICATION_DATA*, PVOID), PVOID, PVOID*);

static void CALLBACK OnDllNotification(ULONG reason, const LDR_DLL_NOTIFICATION_DATA* data, PVOID)
{
    if (reason != 1 /* LOADED */ || !data || !data->BaseDllName) return;
    const UNICODE_STR* n = data->BaseDllName;
    if (n->Length == 16 && _wcsnicmp(n->Buffer, L"d3d9.dll", 8) == 0) OnD3D9Loaded((HMODULE)data->DllBase);
    if (n->Length == 32 && _wcsnicmp(n->Buffer, L"shaderapidx9.dll", 16) == 0) OnShaderApiLoaded((HMODULE)data->DllBase);
}

// ---- crash diagnostics: first-chance access violations with module+offset and the stack --------

static void Where(void* addr, char* out, size_t n)
{
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m) {
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* base = strrchr(name, '\\');
        sprintf_s(out, n, "%s+0x%x", base ? base + 1 : name, (unsigned)((uintptr_t)addr - (uintptr_t)m));
    } else {
        sprintf_s(out, n, "%p", addr);
    }
}

static LONG CALLBACK OnException(EXCEPTION_POINTERS* ep)
{
    static volatile LONG count;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION || InterlockedIncrement(&count) > 5) return EXCEPTION_CONTINUE_SEARCH;
    char at[300], buf[1200];
    Where(ep->ExceptionRecord->ExceptionAddress, at, sizeof(at));
    int len = sprintf_s(buf, "access violation at %s (%s %p), stack:", at,
                        ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                        (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    // Walk EBP frames of the faulting context.
    uintptr_t* ebp = (uintptr_t*)ep->ContextRecord->Ebp;
    for (int i = 0; i < 12 && ebp && !IsBadReadPtr(ebp, 8); ++i) {
        Where((void*)ebp[1], at, sizeof(at));
        len += sprintf_s(buf + len, sizeof(buf) - len, " %s", at);
        if (len > (int)sizeof(buf) - 80) break;
        ebp = (uintptr_t*)ebp[0];
    }
    LOG("%s", buf);
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        xlog::Init(inst);
        AddVectoredExceptionHandler(1, OnException);
        LOG("p2r_core loaded in pid %lu (%s)", GetCurrentProcessId(), GetCommandLineA());
        d3d9ex::Init();
        capture::Init(inst);
        if (GetModuleHandleW(L"shaderapidx9.dll")) {
            LOG("loaded too late: shaderapidx9.dll is already there (the game stays plain D3D9)");
        } else {
            auto reg = (LdrRegisterDllNotification_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
            PVOID cookie = nullptr;
            LONG st = reg ? reg(0, OnDllNotification, nullptr, &cookie) : -1;
            LOG("waiting for shaderapidx9.dll (notification %ld)", st);
        }
    }
    return TRUE;
}
