# Portal 2 reverse-engineering notes (renderer / D3D9)

Addresses are Ghidra analysis addresses: Windows PE image base `0x10000000`, Mac dylib base `0`.
Mac `shaderapidx9.dylib` (E:\P2\MAC\bin) has symbols and goes through togl (GL), but the shader API
code above the D3D9 calls is the same as Windows. Windows `shaderapidx9.dll` is from the current
Steam build (PatchVersion 2.0.0.1, file dated 2026-10-04). Ghidra projects: `mac_shaderapi`,
`win_shaderapi` (see AGENTS.md, RE workflow).

## How the device is created

- `shaderapidx9.dll` imports only `D3D9.DLL!Direct3DCreate9` and `D3DPERF_SetOptions` (no
  `Direct3DCreate9Ex`). With `-vulkan` the engine loads `shaderapivk.dll` + `dxvk_d3d9.dll` instead.
- `CShaderDeviceMgrDx8::Connect`-ish, Windows `FUN_10021b80`: `Direct3DCreate9(0x20)` stored at
  `this+0x30` (`g_ShaderDeviceMgrDx8`), "Failed to create D3D9!" on null.
- `CShaderDeviceDx8::CreateD3DDevice` (Mac 0x33070, Windows `FUN_10023370`): GetDeviceCaps (IDirect3D9
  slot 14), `SetPresentParameters` (Mac 0x32a30, Windows `FUN_100224c0`) fills the
  D3DPRESENT_PARAMETERS at `this+0x2c`, then `InvokeCreateDevice` (Mac 0x33f80, Windows
  `FUN_10023260`) calls IDirect3D9 slot 16 `CreateDevice(adapter, D3DDEVTYPE_HAL, hwnd, flags, pp, &dev)`
  up to 3 times (Sleep(1000) between; 3rd try forces D3DSWAPEFFECT_COPY when windowed).
  Behaviour flags (Windows): `D3DCREATE_FPU_PRESERVE (2) |` `HARDWARE_VERTEXPROCESSING (0x40)`
  `[| PUREDEVICE (0x10)]`, or `SOFTWARE_VERTEXPROCESSING (0x20)`. Observed: `0x52`. No MULTITHREADED.
  The device pointer goes to the global `g_pD3DDevice` (Windows `DAT_100da5d8`).
- Observed present params (-sw -w 1280 -h 720): 1280x720 A8R8G8B8 x1, **MSAA 4x**, SWAPEFFECT_DISCARD,
  windowed, D24S8 auto depth, PresentationInterval IMMEDIATE.

## Pools the shader API uses

- `CreateD3DTexture(w, h, depth, ImageFormat, levels, flags, debugName)` (Mac 0x367a0, Windows
  `FUN_10025c50`, cdecl). From the creation flags: bit0 cube, bit1 render target
  (D3DUSAGE_RENDERTARGET), bit2 **managed** (D3DPOOL_MANAGED, else DEFAULT), bit4 D3DUSAGE_DYNAMIC,
  bit5 D3DUSAGE_AUTOGENMIPMAP, bit16 SYSTEMMEM. Depth formats (INTZ, RAWZ, D16, D24S8, DF16, DF24)
  are forced to DEFAULT + D3DUSAGE_DEPTHSTENCIL. Device slots: 23 CreateTexture (+0x5c),
  24 CreateVolumeTexture (+0x60), 25 CreateCubeTexture (+0x64); pSharedHandle always 0.
- `LockTexture` (Mac 0x36a70): GetSurfaceLevel / GetCubeMapSurface then **surface LockRect with a
  rect**, flags `D3DLOCK_NOSYSLOCK (0x800)` or `| D3DLOCK_DISCARD (0x2800)`. Used by
  `CShaderAPIDX8::BlitTextureBits` ("couldn't lock texture rect" in console on failure).
- Measured in sp_a2_laser_intro: ~1040 managed textures, ~22 managed cubes, 0 volume, ~680 managed
  vertex buffers, **0 managed index buffers**.

## D3D9Ex consequences (what the proxy does, src/d3d9ex.cpp)

- D3DPOOL_MANAGED is invalid on an Ex device. Managed textures -> DEFAULT + D3DUSAGE_DYNAMIC (the only
  lockable DEFAULT textures on Ex). Managed VB/IB -> DEFAULT (DEFAULT buffers are lockable).
- DXT1/DXT5 dynamic textures: LockRect **with a rect** fails (E_INVALIDARG) for mip levels smaller
  than a 4x4 block (1x2, 2x1, 4x2, 8x2...), even when the rect is the whole level. Retrying with
  `pRect = NULL` works. Without that retry: `CShaderAPIDX8::BlitTextureBits: couldn't lock texture
  rect` and ~130 failures per map.
- Exclusive fullscreen needs a D3DDISPLAYMODEEX in CreateDeviceEx / ResetEx (filled from the present
  params).

## DLL search order

`shaderapidx9.dll` is loaded from `bin\` by full path, so its `d3d9.dll` import is searched in `bin\`
first. A `d3d9.dll` next to `portal2.exe` is NOT loaded (verified: the system one was mapped).
`bin\d3d9.dll` is.
