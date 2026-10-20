// p2r_core.dll: loaded into portal2.exe by p2r_launch.exe before the game's main() runs (nothing is
// installed in the game folder). When the system d3d9.dll gets mapped (by shaderapidx9.dll), its
// Direct3DCreate9 is patched to return an IDirect3D9Ex (Direct3DCreate9Ex); d3d9ex.cpp then turns
// the device into a D3D9Ex device and capture.cpp renders the jobs P2Render sends.
#include <windows.h>
#include <d3d9.h>
#include <intrin.h>
#include <stdint.h>
#include <string.h>

#include "capture.h"
#include "d3d9ex.h"
#include "log.h"

// ---- Direct3DCreate9 -> Direct3DCreate9Ex ---------------------------------------------------

using Create9_t = IDirect3D9*(WINAPI*)(UINT);
using Create9Ex_t = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
static Create9_t g_origCreate9;   // trampoline to the original function
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

// 5-byte jmp at the start of Direct3DCreate9. The system d3d9 starts it with the hot-patch prologue
// mov edi,edi / push ebp / mov ebp,esp (8B FF 55 8B EC): those 5 bytes go into the trampoline.
static void HookD3D9(HMODULE d3d9)
{
    static bool done;
    if (done) return;
    done = true;
    uint8_t* fn = (uint8_t*)GetProcAddress(d3d9, "Direct3DCreate9");
    g_create9Ex = (Create9Ex_t)GetProcAddress(d3d9, "Direct3DCreate9Ex");
    static const uint8_t prologue[5] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};
    if (!fn || memcmp(fn, prologue, 5) != 0) {
        LOG("Direct3DCreate9 at %p has an unexpected prologue: not hooked (the game stays plain D3D9)", fn);
        return;
    }
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tramp, fn, 5);
    tramp[5] = 0xE9;
    *(int32_t*)(tramp + 6) = (int32_t)((fn + 5) - (tramp + 10));
    g_origCreate9 = (Create9_t)tramp;
    DWORD prot;
    VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &prot);
    fn[0] = 0xE9;
    *(int32_t*)(fn + 1) = (int32_t)((uint8_t*)&Hook_Direct3DCreate9 - (fn + 5));
    VirtualProtect(fn, 5, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    LOG("Direct3DCreate9 hooked in %p", d3d9);
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
    if (n->Length == 16 && _wcsnicmp(n->Buffer, L"d3d9.dll", 8) == 0) HookD3D9((HMODULE)data->DllBase);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        xlog::Init(inst);
        LOG("p2r_core loaded in pid %lu (%s)", GetCurrentProcessId(), GetCommandLineA());
        d3d9ex::Init();
        capture::Init(inst);
        if (HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll")) {
            HookD3D9(d3d9);
        } else {
            auto reg = (LdrRegisterDllNotification_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
            PVOID cookie = nullptr;
            LONG st = reg ? reg(0, OnDllNotification, nullptr, &cookie) : -1;
            LOG("waiting for d3d9.dll (notification %ld)", st);
        }
    }
    return TRUE;
}
