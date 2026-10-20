# Portal 2 D3D9Ex - developer handbook

Goal: render Portal 2 demos as fast as possible. The plan is to share the game's frames with an
encoder **on the GPU** (texture sharing) instead of reading them back to the CPU (`startmovie`
writes TGA/WAV per frame). Texture sharing (shared handles) needs a **Direct3D 9Ex** device, and
Portal 2 creates a plain Direct3D 9 device, so step 1 is to make the game run on D3D9Ex.

This file is the entry point for every new contributor and every AI session. Read it fully first,
then `re/notes.md`.

## Hard rules (from the project owner)

- **Do not look at, read, or copy from the other plugins in the game folder: `SAR` (`sar.dll`) and
  `FastLoads` (`FastLoads.dll`, `P2Plugin.dll`, `portal2_hello.dll`), nor their logs/configs.**
  This project is independent. That includes `D:\OpenCode\sourceautorecord` and
  `D:\OpenCode\Portal 2 FastLoads`.
- Game install: `E:\Program Files (x86)\Steam\steamapps\common\Portal 2`.
- Symbolised reference builds (use for RE, 99% same code): Mac `E:\P2\MAC`, Linux `E:\P2\Linux`.
- Ghidra: `D:\ghidra\ghidra_12.1.4_PUBLIC`. The REA skill + its MCP tools are available for decomp.
- `D:\OpenCode\ChocolateLoading` (the owner's other Portal 2 project) may be read; its `re/notes.md`
  has engine-wide findings (allocator, frame loop, demo recorder, D3D objects).
- Git: commit every change (small commits, English messages).

## Layout

```
src/
  proxy.cpp      d3d9.dll proxy: loads System32\d3d9.dll, forwards every export;
                 Direct3DCreate9 returns an IDirect3D9Ex (Direct3DCreate9Ex)
  d3d9ex.{h,cpp} the D3D9Ex conversion (vtable hooks on the real d3d9 objects): CreateDevice ->
                 CreateDeviceEx, MANAGED -> DEFAULT(+DYNAMIC), lock retries, stats, -d3d9ex_shot
  log.{h,cpp}    <Portal 2>\d3d9ex.log
  d3d9.def       exports (names of the system d3d9.dll)
re/
  notes.md       ALL RE findings (addresses, slots, evidence) - read this
  ghidra.ps1     headless Ghidra: import / query (P2Query.java commands)
  ghidra_scripts/P2Query.java
  projects/<proj>.progname   name of the program inside each Ghidra project
  bin/ out/      (ignored) copies of the analysed binaries, query outputs
build.ps1        build / -Deploy / -Undeploy
```

## Build, deploy, test

Visual Studio 18 (Community) is installed; no cmake/ninja/python. Node 24 is available.

```powershell
cd "D:\OpenCode\Portal 2 D3D9Ex"
.\build.ps1             # build\d3d9.dll (+ .pdb), x86, /MT, warning-free at /W3
.\build.ps1 -Deploy     # copy to <Portal 2>\bin\d3d9.dll  (NOT next to portal2.exe: see re/notes.md)
.\build.ps1 -Undeploy   # remove it: the game is plain D3D9 again
```

Launch options read by the proxy:

| Option | Effect |
|--------|--------|
| `-nod3d9ex` | proxy loaded but everything passed through (plain D3D9) - baseline for comparisons |
| `-d3d9ex_shot N` | write the back buffer of the N-th Present to `<Portal 2>\d3d9ex_shot.bmp` |

Smoke test (Steam must be running; ~300 fps in the menu, the map is in game around frame 18000):

```powershell
$g = "E:\Program Files (x86)\Steam\steamapps\common\Portal 2"
Start-Process "$g\portal2.exe" -WorkingDirectory $g -ArgumentList "-game portal2 -novid -sw -w 1280 -h 720 -condebug -d3d9ex_shot 25000 +map sp_a2_laser_intro"
```

Expect in `d3d9ex.log`: `Direct3DCreate9(32) ... -> Direct3DCreate9Ex hr=0x00000000`,
`CreateDeviceEx -> 0x00000000`, `stats` lines with `lock failures 0`, and a correct image in
`d3d9ex_shot.bmp`. `portal2\console.log` must not show `couldn't lock texture rect` or
`Failed to lock vertex buffer`. Note: `jpeg`/`screenshot` from a cfg with `wait` captures the
loading screen (also on plain D3D9); use `-d3d9ex_shot` instead.

## How it works

`bin\d3d9.dll` is our proxy. `Direct3DCreate9` calls the real `Direct3DCreate9Ex` and returns that
object (IDirect3D9Ex derives from IDirect3D9), after patching its vtable slot 16 (CreateDevice) so
the device is created with `CreateDeviceEx`. The device vtable (shared by every device d3d9.dll
creates) is patched for: Reset (ResetEx in fullscreen), Present (frame counter, stats, shot),
Create{,Volume,Cube}Texture (MANAGED -> DEFAULT + DYNAMIC, retry static if DYNAMIC is refused),
Create{Vertex,Index}Buffer (MANAGED -> DEFAULT), UpdateSurface/UpdateTexture (failure log). The first
buffer / texture surface created also gives us the VB/IB `Lock` and surface `LockRect` vtables:
LockRect retries without a rect when a full-level lock of a small DXT mip fails (see re/notes.md).
We patch vtables instead of wrapping COM objects so that the Steam overlay and other tools keep
seeing real d3d9 interfaces.

## Status (2026-10-20)

**Step 1 done: Portal 2 runs on a D3D9Ex device.** sp_a2_laser_intro, windowed 1280x720, MSAA 4x:
device created with CreateDeviceEx, ~1040 textures / ~680 vertex buffers converted from MANAGED,
0 lock failures, image identical in nature to plain D3D9 (`-d3d9ex_shot`). Not yet tested:
exclusive fullscreen, alt-tab / Reset, a full play-through, video mode changes, performance compared
to plain D3D9, AMD/Intel drivers (tested on NVIDIA).

## Next steps (plan)

1. **Shared capture texture.** In Present: `StretchRect` the (multisampled) back buffer into a
   non-MSAA render target created with `pSharedHandle` (ring of N, BGRA8), issue a
   D3DQUERYTYPE_EVENT so the consumer knows when the copy is done. No CPU readback.
2. **Encoder process (x64)** opens the shared handles with D3D11 `OpenSharedResource` and encodes on
   the GPU (Media Foundation hardware H.264/HEVC MFT with a DXGI device manager - no external
   dependency - or NVENC / ffmpeg if the owner prefers). Frame hand-off through a small shared-memory
   ring + events; the game must never wait unless the ring is full.
3. **Demo driving.** `playdemo` with a fixed `host_framerate` (= output fps), `fps_max 0`,
   `mat_vsync 0`; one Present = one output frame. Skip loading / menu frames (engine state, see
   ChocolateLoading notes for the demo player / frame loop). Audio: the engine mixes itself; options
   are `startmovie`'s WAV output alone or hooking the mixer output.
4. Validate speed vs `startmovie` and the image vs a reference.
