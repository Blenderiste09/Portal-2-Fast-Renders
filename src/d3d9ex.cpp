#include "d3d9ex.h"

#include <windows.h>
#include <d3d9.h>
#include <intrin.h>
#include <stdlib.h>
#include <string.h>

#include "capture.h"
#include "log.h"

namespace d3d9ex {

// Vtable slots (d3d9.h declaration order).
enum {
    D3D9_CreateDevice = 16,

    DEV_Reset = 16,
    DEV_Present = 17,
    DEV_CreateTexture = 23,
    DEV_CreateVolumeTexture = 24,
    DEV_CreateCubeTexture = 25,
    DEV_CreateVertexBuffer = 26,
    DEV_CreateIndexBuffer = 27,
    DEV_UpdateSurface = 30,
    DEV_UpdateTexture = 31,

    BUF_Lock = 11,     // IDirect3DVertexBuffer9 / IDirect3DIndexBuffer9
    SURF_LockRect = 13 // IDirect3DSurface9
};

static bool g_enabled = true;
static IDirect3DDevice9Ex* g_device;

struct Stats {
    volatile LONG texConverted, texFallback, cubeConverted, volConverted;
    volatile LONG vbConverted, ibConverted;
    volatile LONG lockFail, lockRetryOk, updateFail;
};
static Stats g_stats;
static volatile LONG g_frames;
static long g_shotFrame = -1; // -d3d9ex_shot <frame>

bool Enabled() { return g_enabled; }
IDirect3DDevice9Ex* Device() { return g_device; }

void Init()
{
    const char* cmd = GetCommandLineA();
    if (strstr(cmd, "-nod3d9ex")) g_enabled = false;
    if (const char* s = strstr(cmd, "-d3d9ex_shot ")) g_shotFrame = atol(s + 13);
    LOG("d3d9ex %s", g_enabled ? "enabled" : "disabled by -nod3d9ex");
}

// Replaces vtable[slot] of obj with fn and returns the previous entry. Patching the same vtable
// twice returns our own function the second time, so callers keep the first original.
static void* PatchSlot(void* obj, int slot, void* fn)
{
    void** vt = *(void***)obj;
    void* old = vt[slot];
    if (old == fn) return nullptr;
    DWORD prot;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &prot)) {
        LOG("VirtualProtect failed on vtable %p slot %d (%lu)", vt, slot, GetLastError());
        return nullptr;
    }
    vt[slot] = fn;
    VirtualProtect(&vt[slot], sizeof(void*), prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), &vt[slot], sizeof(void*));
    return old;
}

#define HOOK(obj, slot, fn, orig)                                     \
    do {                                                              \
        void* o = PatchSlot(obj, slot, (void*)&fn);                   \
        if (o && !orig) orig = (decltype(orig))o;                     \
    } while (0)

static const char* PoolName(D3DPOOL p)
{
    switch (p) {
    case D3DPOOL_DEFAULT: return "DEFAULT";
    case D3DPOOL_MANAGED: return "MANAGED";
    case D3DPOOL_SYSTEMMEM: return "SYSTEMMEM";
    case D3DPOOL_SCRATCH: return "SCRATCH";
    default: return "?";
    }
}

// ---------------------------------------------------------------------------------------------
// Resource locks: only observed, and retried without DISCARD/NOOVERWRITE when a converted
// (non-dynamic) buffer refuses them.

using BufLock_t = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, UINT, void**, DWORD);
static BufLock_t o_VBLock, o_IBLock;
using SurfLockRect_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSurface9*, D3DLOCKED_RECT*, const RECT*, DWORD);
static SurfLockRect_t o_SurfLockRect;

static HRESULT BufLock(BufLock_t orig, const char* what, void* self, UINT off, UINT size, void** data, DWORD flags)
{
    HRESULT hr = orig(self, off, size, data, flags);
    if (FAILED(hr)) {
        DWORD stripped = flags & ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);
        HRESULT hr2 = stripped != flags ? orig(self, off, size, data, stripped) : hr;
        if (SUCCEEDED(hr2)) {
            if (InterlockedIncrement(&g_stats.lockRetryOk) <= 20)
                LOG("%s Lock(off %u, size %u, flags 0x%lx) failed 0x%08lx, ok without DISCARD/NOOVERWRITE", what, off, size, flags, hr);
            return hr2;
        }
        if (InterlockedIncrement(&g_stats.lockFail) <= 50)
            LOG("%s Lock(off %u, size %u, flags 0x%lx) failed 0x%08lx", what, off, size, flags, hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_VBLock(void* self, UINT off, UINT size, void** data, DWORD flags)
{
    return BufLock(o_VBLock, "VB", self, off, size, data, flags);
}

static HRESULT STDMETHODCALLTYPE Hook_IBLock(void* self, UINT off, UINT size, void** data, DWORD flags)
{
    return BufLock(o_IBLock, "IB", self, off, size, data, flags);
}

static HRESULT STDMETHODCALLTYPE Hook_SurfLockRect(IDirect3DSurface9* self, D3DLOCKED_RECT* lr, const RECT* r, DWORD flags)
{
    HRESULT hr = o_SurfLockRect(self, lr, r, flags);
    if (FAILED(hr) && r) {
        // A DXT level smaller than a 4x4 block (2x1, 4x2...) cannot be locked with a rect in the
        // DEFAULT pool, even when the rect is the whole level (MANAGED accepted it). Locking the
        // whole level without a rect works.
        D3DSURFACE_DESC d = {};
        self->GetDesc(&d);
        if (r->left == 0 && r->top == 0 && (UINT)r->right == d.Width && (UINT)r->bottom == d.Height) {
            HRESULT hr2 = o_SurfLockRect(self, lr, nullptr, flags);
            if (SUCCEEDED(hr2)) {
                InterlockedIncrement(&g_stats.lockRetryOk);
                return hr2;
            }
        }
    }
    if (FAILED(hr) && InterlockedIncrement(&g_stats.lockFail) <= 50) {
        D3DSURFACE_DESC d = {};
        self->GetDesc(&d);
        LOG("surface LockRect(flags 0x%lx) failed 0x%08lx: %ux%u fmt %d usage 0x%lx pool %s",
            flags, hr, d.Width, d.Height, d.Format, d.Usage, PoolName(d.Pool));
    }
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Device

using CreateTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
using CreateVolumeTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*);
using CreateCubeTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
using CreateVertexBuffer_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
using CreateIndexBuffer_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using UpdateSurface_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const POINT*);
using UpdateTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DBaseTexture9*, IDirect3DBaseTexture9*);

static CreateTexture_t o_CreateTexture;
static CreateVolumeTexture_t o_CreateVolumeTexture;
static CreateCubeTexture_t o_CreateCubeTexture;
static CreateVertexBuffer_t o_CreateVertexBuffer;
static CreateIndexBuffer_t o_CreateIndexBuffer;
static Reset_t o_Reset;
static Present_t o_Present;
static UpdateSurface_t o_UpdateSurface;
static UpdateTexture_t o_UpdateTexture;

// Usage for a texture that was MANAGED: lockable from the CPU (DYNAMIC), unless the texture is a
// render target / depth buffer (never managed in practice).
static DWORD ManagedTextureUsage(DWORD usage)
{
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return usage;
    return usage | D3DUSAGE_DYNAMIC;
}

static void HookSurfaceVtable(IDirect3DTexture9* tex)
{
    if (o_SurfLockRect || !tex) return;
    IDirect3DSurface9* s = nullptr;
    if (SUCCEEDED(tex->GetSurfaceLevel(0, &s)) && s) {
        HOOK(s, SURF_LockRect, Hook_SurfLockRect, o_SurfLockRect);
        s->Release();
    }
}

static HRESULT STDMETHODCALLTYPE Hook_CreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage,
                                                    D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared)
{
    if (pool != D3DPOOL_MANAGED) return o_CreateTexture(dev, w, h, levels, usage, fmt, pool, out, shared);
    DWORD u = ManagedTextureUsage(usage);
    HRESULT hr = o_CreateTexture(dev, w, h, levels, u, fmt, D3DPOOL_DEFAULT, out, shared);
    if (FAILED(hr) && u != usage) {
        // e.g. a format the driver cannot make dynamic: keep it static (then it cannot be locked).
        HRESULT hr2 = o_CreateTexture(dev, w, h, levels, usage, fmt, D3DPOOL_DEFAULT, out, shared);
        if (InterlockedIncrement(&g_stats.texFallback) <= 50)
            LOG("CreateTexture %ux%u lv %u fmt %d usage 0x%lx: DYNAMIC failed 0x%08lx, static -> 0x%08lx",
                w, h, levels, fmt, usage, hr, hr2);
        hr = hr2;
    } else if (FAILED(hr)) {
        LOG("CreateTexture %ux%u lv %u fmt %d usage 0x%lx MANAGED->DEFAULT failed 0x%08lx", w, h, levels, fmt, usage, hr);
    }
    if (SUCCEEDED(hr)) {
        if (InterlockedIncrement(&g_stats.texConverted) == 1) HookSurfaceVtable(*out);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateVolumeTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT d, UINT levels, DWORD usage,
                                                          D3DFORMAT fmt, D3DPOOL pool, IDirect3DVolumeTexture9** out, HANDLE* shared)
{
    if (pool != D3DPOOL_MANAGED) return o_CreateVolumeTexture(dev, w, h, d, levels, usage, fmt, pool, out, shared);
    DWORD u = ManagedTextureUsage(usage);
    HRESULT hr = o_CreateVolumeTexture(dev, w, h, d, levels, u, fmt, D3DPOOL_DEFAULT, out, shared);
    if (FAILED(hr) && u != usage) {
        HRESULT hr2 = o_CreateVolumeTexture(dev, w, h, d, levels, usage, fmt, D3DPOOL_DEFAULT, out, shared);
        LOG("CreateVolumeTexture %ux%ux%u fmt %d: DYNAMIC failed 0x%08lx, static -> 0x%08lx", w, h, d, fmt, hr, hr2);
        hr = hr2;
    }
    if (SUCCEEDED(hr)) InterlockedIncrement(&g_stats.volConverted);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCubeTexture(IDirect3DDevice9* dev, UINT edge, UINT levels, DWORD usage,
                                                        D3DFORMAT fmt, D3DPOOL pool, IDirect3DCubeTexture9** out, HANDLE* shared)
{
    if (pool != D3DPOOL_MANAGED) return o_CreateCubeTexture(dev, edge, levels, usage, fmt, pool, out, shared);
    DWORD u = ManagedTextureUsage(usage);
    HRESULT hr = o_CreateCubeTexture(dev, edge, levels, u, fmt, D3DPOOL_DEFAULT, out, shared);
    if (FAILED(hr) && u != usage) {
        HRESULT hr2 = o_CreateCubeTexture(dev, edge, levels, usage, fmt, D3DPOOL_DEFAULT, out, shared);
        LOG("CreateCubeTexture %u fmt %d: DYNAMIC failed 0x%08lx, static -> 0x%08lx", edge, fmt, hr, hr2);
        hr = hr2;
    }
    if (SUCCEEDED(hr)) InterlockedIncrement(&g_stats.cubeConverted);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateVertexBuffer(IDirect3DDevice9* dev, UINT len, DWORD usage, DWORD fvf,
                                                         D3DPOOL pool, IDirect3DVertexBuffer9** out, HANDLE* shared)
{
    bool conv = pool == D3DPOOL_MANAGED;
    HRESULT hr = o_CreateVertexBuffer(dev, len, usage, fvf, conv ? D3DPOOL_DEFAULT : pool, out, shared);
    if (SUCCEEDED(hr) && *out && !o_VBLock) HOOK(*out, BUF_Lock, Hook_VBLock, o_VBLock);
    if (conv) {
        if (SUCCEEDED(hr)) InterlockedIncrement(&g_stats.vbConverted);
        else LOG("CreateVertexBuffer %u usage 0x%lx MANAGED->DEFAULT failed 0x%08lx", len, usage, hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateIndexBuffer(IDirect3DDevice9* dev, UINT len, DWORD usage, D3DFORMAT fmt,
                                                        D3DPOOL pool, IDirect3DIndexBuffer9** out, HANDLE* shared)
{
    bool conv = pool == D3DPOOL_MANAGED;
    HRESULT hr = o_CreateIndexBuffer(dev, len, usage, fmt, conv ? D3DPOOL_DEFAULT : pool, out, shared);
    if (SUCCEEDED(hr) && *out && !o_IBLock) HOOK(*out, BUF_Lock, Hook_IBLock, o_IBLock);
    if (conv) {
        if (SUCCEEDED(hr)) InterlockedIncrement(&g_stats.ibConverted);
        else LOG("CreateIndexBuffer %u usage 0x%lx MANAGED->DEFAULT failed 0x%08lx", len, usage, hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_UpdateSurface(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* r,
                                                    IDirect3DSurface9* dst, const POINT* p)
{
    HRESULT hr = o_UpdateSurface(dev, src, r, dst, p);
    if (FAILED(hr) && InterlockedIncrement(&g_stats.updateFail) <= 50) LOG("UpdateSurface failed 0x%08lx", hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_UpdateTexture(IDirect3DDevice9* dev, IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst)
{
    HRESULT hr = o_UpdateTexture(dev, src, dst);
    if (FAILED(hr) && InterlockedIncrement(&g_stats.updateFail) <= 50) LOG("UpdateTexture failed 0x%08lx", hr);
    return hr;
}

static void LogStats(const char* when)
{
    LOG("stats (%s): frames %ld, converted tex %ld (static fallback %ld) cube %ld vol %ld vb %ld ib %ld; "
        "lock failures %ld (retried ok %ld), update failures %ld",
        when, g_frames, g_stats.texConverted, g_stats.texFallback, g_stats.cubeConverted, g_stats.volConverted,
        g_stats.vbConverted, g_stats.ibConverted, g_stats.lockFail, g_stats.lockRetryOk, g_stats.updateFail);
}

static void FillMode(const D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode)
{
    memset(mode, 0, sizeof(*mode));
    mode->Size = sizeof(*mode);
    mode->Width = pp->BackBufferWidth;
    mode->Height = pp->BackBufferHeight;
    mode->RefreshRate = pp->FullScreen_RefreshRateInHz;
    mode->Format = pp->BackBufferFormat;
    mode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
}

static void LogPP(const char* what, const D3DPRESENT_PARAMETERS* pp)
{
    LOG("%s: %ux%u fmt %d x%u, ms %d/%lu, swap %d, windowed %d, depth %d/%d, flags 0x%lx, %u Hz, interval 0x%x",
        what, pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->BackBufferCount,
        pp->MultiSampleType, pp->MultiSampleQuality, pp->SwapEffect, pp->Windowed,
        pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat, pp->Flags, pp->FullScreen_RefreshRateInHz,
        pp->PresentationInterval);
}

static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    LogStats("before Reset");
    LogPP("Reset", pp);
    HRESULT hr;
    IDirect3DDevice9Ex* ex = nullptr;
    if (!pp->Windowed && SUCCEEDED(dev->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&ex)) && ex) {
        D3DDISPLAYMODEEX mode;
        FillMode(pp, &mode);
        hr = ex->ResetEx(pp, &mode);
        ex->Release();
    } else {
        hr = o_Reset(dev, pp);
    }
    LOG("Reset -> 0x%08lx", hr);
    return hr;
}

// Diagnostic: -d3d9ex_shot <frame> writes the back buffer of that Present to d3d9ex_shot.bmp
// (next to portal2.exe). Goes through a resolve (the back buffer may be multisampled) and a
// system-memory copy, i.e. the slow path the shared-texture capture is meant to replace.

static void SaveBackBuffer(IDirect3DDevice9* dev, const char* path)
{
    IDirect3DSurface9 *bb = nullptr, *rt = nullptr, *sys = nullptr;
    D3DSURFACE_DESC d = {};
    HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (SUCCEEDED(hr)) hr = bb->GetDesc(&d);
    if (SUCCEEDED(hr)) hr = dev->CreateRenderTarget(d.Width, d.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr);
    if (SUCCEEDED(hr)) hr = dev->StretchRect(bb, nullptr, rt, nullptr, D3DTEXF_NONE);
    if (SUCCEEDED(hr)) hr = dev->CreateOffscreenPlainSurface(d.Width, d.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (SUCCEEDED(hr)) hr = dev->GetRenderTargetData(rt, sys);
    D3DLOCKED_RECT lr = {};
    if (SUCCEEDED(hr)) hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (SUCCEEDED(hr)) {
        BITMAPFILEHEADER fh = {};
        BITMAPINFOHEADER ih = {};
        ih.biSize = sizeof(ih);
        ih.biWidth = d.Width;
        ih.biHeight = -(LONG)d.Height;
        ih.biPlanes = 1;
        ih.biBitCount = 32;
        DWORD bytes = d.Width * 4 * d.Height;
        fh.bfType = 0x4d42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + bytes;
        HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(f, &fh, sizeof(fh), &w, nullptr);
            WriteFile(f, &ih, sizeof(ih), &w, nullptr);
            for (UINT y = 0; y < d.Height; ++y)
                WriteFile(f, (BYTE*)lr.pBits + y * lr.Pitch, d.Width * 4, &w, nullptr);
            CloseHandle(f);
        }
        sys->UnlockRect();
    }
    LOG("back buffer %ux%u fmt %d ms %d -> %s: 0x%08lx", d.Width, d.Height, d.Format, d.MultiSampleType, path, hr);
    if (sys) sys->Release();
    if (rt) rt->Release();
    if (bb) bb->Release();
}

static HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty)
{
    LONG f = InterlockedIncrement(&g_frames);
    if (f == 1 || f % 3000 == 0) LogStats("present");
    if (f == g_shotFrame) {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
        while (n > 0 && path[n - 1] != '\\') --n;
        strcpy_s(path + n, MAX_PATH - n, "d3d9ex_shot.bmp");
        SaveBackBuffer(dev, path);
    }
    capture::OnPresentHooked();
    if (capture::SkipPresent()) return S_OK;
    return o_Present(dev, src, dst, wnd, dirty);
}

static void HookDevice(IDirect3DDevice9Ex* dev)
{
    HOOK(dev, DEV_Reset, Hook_Reset, o_Reset);
    HOOK(dev, DEV_Present, Hook_Present, o_Present);
    HOOK(dev, DEV_CreateTexture, Hook_CreateTexture, o_CreateTexture);
    HOOK(dev, DEV_CreateVolumeTexture, Hook_CreateVolumeTexture, o_CreateVolumeTexture);
    HOOK(dev, DEV_CreateCubeTexture, Hook_CreateCubeTexture, o_CreateCubeTexture);
    HOOK(dev, DEV_CreateVertexBuffer, Hook_CreateVertexBuffer, o_CreateVertexBuffer);
    HOOK(dev, DEV_CreateIndexBuffer, Hook_CreateIndexBuffer, o_CreateIndexBuffer);
    HOOK(dev, DEV_UpdateSurface, Hook_UpdateSurface, o_UpdateSurface);
    HOOK(dev, DEV_UpdateTexture, Hook_UpdateTexture, o_UpdateTexture);
}

// ---------------------------------------------------------------------------------------------
// IDirect3D9Ex::CreateDevice -> CreateDeviceEx

using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static CreateDevice_t o_CreateDevice;

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
                                                   D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    IDirect3D9Ex* ex = nullptr;
    if (FAILED(d3d->QueryInterface(__uuidof(IDirect3D9Ex), (void**)&ex)) || !ex) {
        LOG("CreateDevice on a non-Ex IDirect3D9: passed through");
        return o_CreateDevice(d3d, adapter, type, wnd, flags, pp, out);
    }
    LOG("CreateDevice(adapter %u, type %d, hwnd %p, flags 0x%lx) from %p", adapter, type, wnd, flags, _ReturnAddress());
    LogPP("present params", pp);
    D3DDISPLAYMODEEX mode;
    FillMode(pp, &mode);
    IDirect3DDevice9Ex* dev = nullptr;
    HRESULT hr = ex->CreateDeviceEx(adapter, type, wnd, flags, pp, pp->Windowed ? nullptr : &mode, &dev);
    ex->Release();
    LOG("CreateDeviceEx -> 0x%08lx, device %p", hr, dev);
    if (FAILED(hr) || !dev) {
        *out = nullptr;
        return hr;
    }
    HookDevice(dev);
    if (!g_device) g_device = dev;
    *out = dev; // IDirect3DDevice9Ex derives from IDirect3DDevice9
    return hr;
}

void HookD3D9Ex(IDirect3D9Ex* d3d)
{
    HOOK(d3d, D3D9_CreateDevice, Hook_CreateDevice, o_CreateDevice);
}

} // namespace d3d9ex
