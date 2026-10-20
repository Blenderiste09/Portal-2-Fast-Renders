# Portal 2 D3D9Ex - developer handbook

Goal: render Portal 2 demos as fast as possible, as an **independent tool** (owner's decision,
2026-10-20: not inside SAR, not with SourceDemoRender, which dropped 32-bit games). Frames are shared
with an encoder **on the GPU** (texture sharing) instead of being read back to the CPU like the
engine's `startmovie`. Texture sharing needs a **Direct3D 9Ex** device; Portal 2 creates plain D3D9,
so a `d3d9.dll` proxy switches it to D3D9Ex. User-facing instructions (French): `README.md`.

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
- Git: commit every change (small commits, English messages). The owner talks French.

## Layout

```
src/                 build\d3d9.dll (x86), deployed to <Portal 2>\bin
  proxy.cpp          d3d9.dll proxy: loads System32\d3d9.dll, forwards every export;
                     Direct3DCreate9 returns an IDirect3D9Ex (Direct3DCreate9Ex)
  d3d9ex.{h,cpp}     the D3D9Ex conversion (vtable hooks on the real d3d9 objects): CreateDevice ->
                     CreateDeviceEx, MANAGED -> DEFAULT(+DYNAMIC), lock retries, stats, -d3d9ex_shot,
                     Present hook (calls capture)
  engine.{h,cpp}     engine access: ICvar commands, IVEngineClient, CVideoMode::WriteMovieFrame hook,
                     client FrameStageNotify hook (main-thread callbacks)
  capture.{h,cpp}    the renderer: p2r_start / p2r_stop / p2r_after, shared texture ring, encoder
                     process, demo start/end detection
  shared.h           shared-memory protocol game <-> encoder (also included by the encoder)
  log.{h,cpp}        <Portal 2>\d3d9ex.log
  d3d9.def           exports (names of the system d3d9.dll)
encoder/encoder.cpp  build\encoder\p2r_encoder.exe (x64), deployed next to d3d9.dll
tools/preview/       node server + page to check a rendered .mp4 in the browser pane
re/
  notes.md           ALL RE findings (addresses, slots, evidence) - read this
  ghidra.ps1         headless Ghidra: import / query (P2Query.java commands)
  ghidra_scripts/P2Query.java
  projects/<proj>.progname   name of the program inside each Ghidra project
  bin/ out/          (ignored) copies of the analysed binaries, query outputs
build.ps1            build / -Deploy / -Undeploy
```

## Build, deploy, test

Visual Studio 18 (Community) is installed; no cmake/ninja/python/ffmpeg. Node 24 is available.

```powershell
cd "D:\OpenCode\Portal 2 D3D9Ex"
.\build.ps1             # build\d3d9.dll (x86, /MT) + build\encoder\p2r_encoder.exe (x64), warning-free at /W3
.\build.ps1 -Deploy     # copy both to <Portal 2>\bin  (NOT next to portal2.exe: see re/notes.md)
.\build.ps1 -Undeploy   # remove them: the game is plain D3D9 again
```

Launch options read by the proxy:

| Option | Effect |
|--------|--------|
| `-nod3d9ex` | proxy loaded but everything passed through (plain D3D9) - baseline for comparisons |
| `-d3d9ex_shot N` | write the back buffer of the N-th Present to `<Portal 2>\d3d9ex_shot.bmp` |
| `-p2r_exec <cfg>` | exec a cfg once our commands are registered (`+exec` runs too early) |

Console: `p2r_start <name> [demo=<demo>] [fps=60] [codec=h264|hevc] [bitrate=<kbit/s>] [window=1|0]`,
`p2r_stop`, `p2r_after <frames> <command...>` (Portal 2 has no `wait` command).

**Render test** (Steam running). `portal2\p2r_ref.dem` is a 12 s demo recorded with the current
build (old demos in the folder fail with `DataTable_ParseClientTablesFromBuffer`; record a new one
with `-p2r_exec` + `map ...` + `p2r_after N record x` ... `p2r_after M stop` if it is gone):

```powershell
$g = "E:\Program Files (x86)\Steam\steamapps\common\Portal 2"
Set-Content "$g\portal2\cfg\p2r_test.cfg" "p2r_start test demo=p2r_ref window=0" -Encoding ascii
Start-Process "$g\portal2.exe" -WorkingDirectory $g -ArgumentList "-game portal2 -novid -sw -w 1280 -h 720 -condebug -p2r_exec p2r_test"
```

Expect in `d3d9ex.log`: `WriteMovieFrame hooked`, `shared ring: 8 x WxH`, `encoder started`,
`demo finished: 730 frames (12.2 s of video) rendered in ~1.3 s`, `video written`; in
`bin\p2r_encoder.log`: `video done`, `muxed with ...wav`. Check the file with `tools/preview`
(copy it as `tools\preview\test.mp4`, `preview_start "preview"`): duration ~12.17 s, frames of the
demo, and an audio track (decode it with WebAudio in the page to check it is not silent).

## How it works

**D3D9Ex.** `bin\d3d9.dll` is our proxy. `Direct3DCreate9` calls the real `Direct3DCreate9Ex` and
returns that object, after patching its vtable slot 16 (CreateDevice) so the device is created with
`CreateDeviceEx`. The device vtable (shared by every device d3d9.dll creates) is patched for: Reset
(ResetEx in fullscreen), Present, Create{,Volume,Cube}Texture (MANAGED -> DEFAULT + DYNAMIC),
Create{Vertex,Index}Buffer (MANAGED -> DEFAULT), UpdateSurface/UpdateTexture (failure log); surface
LockRect retries without a rect when a full-level lock of a small DXT mip fails. Vtables are patched
instead of wrapping COM objects so overlays keep seeing real d3d9 interfaces.

**Engine hooks** (main thread). The first Present hooks `IBaseClientDLL::FrameStageNotify`; on the
first FRAME_START the console commands are registered (our ConCommands reuse the vtable of `echo`)
and `CVideoMode::WriteMovieFrame` (videomode vtable slot 23, found from its call site in
CL_TakeSnapshotAndSwap by byte pattern) is replaced.

**Render.** `p2r_start` saves and sets `mat_queue_mode 0; fps_max 0; host_framerate <fps>`, runs
`playdemo`, and once the demo is in game (IsPlayingDemo && IsInGame && !loading, 2 frames) runs
`startmovie p2r_tmp_<name> wav`. The engine then paces the frames (fixed step) and writes the audio
to `portal2\p2r_tmp_<name>.wav`; its video path (`WriteMovieFrame`, which reads the back buffer to
the CPU even for wav-only movies) is replaced by ours: `StretchRect` back buffer (MSAA resolve) ->
one of 8 shared `A8R8G8B8` render-target textures (frame n -> slot n % 8), D3D9 EVENT query; once the
query completes the frame is published (semaphore `ready`). The game blocks only when the 8 slots
are taken (semaphore `free`). `p2r_encoder.exe` opens the shared handles with D3D11
(`OpenSharedResource`), `CopyResource`s each slot to a new texture, releases the slot, and writes it
to a Media Foundation sink writer with hardware transforms and the DXGI device manager (ARGB32 input,
H.264 High or HEVC). At the end (demo not playing for 10 frames, or `p2r_stop`): `endmovie`, cvars
restored, encoder told the frame count; it finalizes the video, waits for the WAV to be closed, and
muxes video (copied) + WAV (AAC 192k) into `<Portal 2>\p2r_movies\<name>.mp4` with a second sink
writer. Encoder log: `<Portal 2>\bin\p2r_encoder.log`.

## Status (2026-10-20)

**Working end to end.** p2r_ref (12.2 s, 60 fps) -> MP4 with sound, checked visually and by decoding
the audio. Speed (Ryzen 7 5700X, NVIDIA, MSAA 4x): 720p 377 fps with the window updated, 550 fps
with `window=0`; 1080p `window=0` 537 fps. Engine `startmovie raw` (TGA): ~126 fps, before encoding.

Not yet tested / ideas:
- Long demos, demos with level changes (loading in the middle: movie frames continue? sync of the
  WAV across the load), co-op demos, exclusive fullscreen, AMD/Intel encoders.
- The encoder creates a texture per frame (released by the sink writer); a pool would save
  allocations. The game's bottleneck at ~550 fps is the game itself (rendering + engine frame).
- Quality: bitrate default ~0.33 bit/pixel at the given fps; no CQP/quality mode yet.
- `startmovie` must not already be running; old demos from before the game update do not load
  (DataTable error), nothing to do with this tool.
