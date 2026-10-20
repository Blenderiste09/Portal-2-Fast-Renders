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

## Engine pieces used by the renderer (Windows engine.dll, same as ChocolateLoading's win_engine)

- **CL_TakeSnapshotAndSwap** = `engine!0x10097ae0` (Mac `CL_TakeSnapshotAndSwap` 0x149af0): after
  screenshots, `if (cl_movieinfo.name[0] && !console_visible() && !...) { videomode->WriteMovieFrame
  (&cl_movieinfo); cl_movieinfo.frame++; }` then Shader_SwapBuffers (Present). Call site at
  0x10097e82: `8B 0D <videomode> 8B 11 8B 42 5C 68 <cl_movieinfo> FF D0 FF 05 <frame>` (unique
  pattern; `videomode` global 0x106b5d38, cl_movieinfo 0x10433d28: name[0x100], +0x100 frame,
  +0x104 flags).
- **CVideoMode_Common::WriteMovieFrame** (vtable slot 23, Mac 0x323fe0) **always** reads the back
  buffer to the CPU (`ReadScreenPixels`, w*h*3 buffer) and only then checks the flags (1 TGA,
  0x10 JPG, 2 AVI). `startmovie x wav` therefore still pays the readback: we replace the slot.
- **startmovie** command = 0x10098250, endmovie = 0x10098510 (CL_StopMovie 0x100981c0), StartMovie
  0x10097f50: movie fps = `host_framerate` value (30 when 0), flags: 4 = wav (`<name>.WAV` in the game
  dir, 44100 Hz stereo 16 bit). The WAV is written while the movie runs; frames are only written
  when the console is hidden.
- **CEngine::FilterTime(float dt, float& minFrameTime)** = 0x101f9d40 (thiscall, `this` unused, ret 8).
- IVEngineClient (vtable 0x103553f4) slots used: 25 IsInGame, 27 IsDrawingLoadingImage, **78
  IsPlayingDemo** (0x101b68e0 -> demoplayer vtable +0x18; the Mac demoplayer slot is +0x1c: MSVC has
  one destructor slot less), 79 IsRecordingDemo, 80 IsPlayingTimeDemo, 104 ExecuteClientCmd
  (0x10079d20: Cbuf_AddText + Cbuf_Execute), 108 ClientCmd_Unrestricted.
- `wait` does not exist in Portal 2 ("Unknown command"); `+exec` on the command line runs before the
  client frame hook, so our commands are not yet registered then (hence `-p2r_exec`, `p2r_after`).
- Demos recorded before the 2026 game update fail with `DataTable_ParseClientTablesFromBuffer`.
