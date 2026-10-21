# P2Render (Portal 2 D3D9Ex) - developer handbook

Goal: render Portal 2 demos as fast as possible with a **standalone application** that has a modern,
intuitive interface. Design decisions (2026-10-20):
- **external tool**: no plugin to load and no console command in the game (SourceDemoRender, the
  usual alternative, dropped 32-bit games; this one keeps them);
- nothing installed in the game folder.

Frames are shared with the encoder **on the GPU** (texture sharing), so they are never read back to
the CPU like the engine's `startmovie`. Texture sharing needs **Direct3D 9Ex**, and Portal 2 creates
plain D3D9, so the injected core switches the device to D3D9Ex. User instructions: `README.md`.

This file is the entry point for every new contributor and every AI session. Read it fully first,
then `re/notes.md`.

## Project rules

- **Independent implementation.** This project shares no code with any other Portal 2 plugin or
  rendering tool: do not copy sources, binaries, logs or configs from them. Everything here was
  written for P2Render or reverse engineered from the game itself (`re/notes.md`).
- Nothing is installed, patched or left behind in the game folder: the injected module lives next to
  `P2Render.exe` and is loaded for the time of a render only.
- Git: commit every change (small commits, English messages). The public documentation (`README.md`,
  `AGENTS.md`) and the interface (`app/ui/index.html`, the messages it receives from the app and the
  core) are in English; the owner works in French.

Local setup of the development machine (not needed to build or use the project):

- Game install: `E:\Program Files (x86)\Steam\steamapps\common\Portal 2`.
- Symbolised reference builds (use for RE, 99% same code): Mac `E:\P2\MAC`, Linux `E:\P2\Linux`.
- Windows dlls used for RE are copied into `re/bin/win` (git-ignored).
- Ghidra: `D:\ghidra\ghidra_12.1.4_PUBLIC`. The REA skill + its MCP tools are available for decomp.

## Layout

```
app/                  P2Render.exe (x64): local HTTP server opened in the default browser,
                      queue, settings
  main.cpp            settings (<exe>\data\settings.ini, portable), Steam library lookup, demo
                      header parsing, job worker, native file dialogs, JSON API
  ui/index.html       the whole interface (vanilla HTML/CSS/JS, polls /api/state every 500 ms);
                      hls.min.js (vendored) plays the live preview stream
launcher/launch.cpp   p2r_launch.exe (x86): starts portal2.exe suspended, parks it on its entry point,
                      LoadLibraryW(p2r_core.dll) through a remote thread, resumes
src/                  p2r_core.dll (x86), injected into the game
  core.cpp            DllMain: LdrRegisterDllNotification; when shaderapidx9.dll is mapped its entry
                      point is redirected once to patch its IAT entry d3d9!Direct3DCreate9 (after the
                      loader bound it); crash logger (vectored handler)
  d3d9ex.{h,cpp}      the D3D9Ex conversion (vtable hooks): CreateDevice -> CreateDeviceEx,
                      MANAGED -> DEFAULT(+DYNAMIC), lock retries, stats, Present hook
  engine.{h,cpp}      engine access: cvars, IVEngineClient, CVideoMode::WriteMovieFrame hook,
                      client FrameStageNotify hook (main-thread callbacks)
  capture.{h,cpp}     job state machine (control block), shared texture ring, encoder process
  control.h           app <-> core protocol (Local\p2render_ctrl_<game pid>)
  shared.h            core <-> encoder protocol (Local\p2r_<id>_*)
  log.{h,cpp}         p2r_core.log next to the DLL
encoder/encoder.cpp   p2r_encoder.exe (x64): D3D11 OpenSharedResource + Media Foundation sink writer
                      (hardware MFT), then mux of the engine WAV (AAC) into the final MP4
tools/preview/        node server (node tools/preview/serve.js, port 8765) + page to check a
                      rendered .mp4 in the browser pane
re/                   notes.md (ALL RE findings), ghidra.ps1 + P2Query.java, projects/*.progname
build.cmd             builds devBuild\ (development) and P2Render\ (the folder to hand out)
```

## Build and test

Visual Studio 18 (Community) is installed; no cmake/ninja/python/ffmpeg/.NET SDK. Node 24 is
available. The interface runs in the user's default browser (no embedded browser runtime).

```bat
cd /d "D:\OpenCode\Portal 2 D3D9Ex"
build.cmd             :: devBuild\: P2Render.exe, p2r_launch.exe, p2r_core.dll(+pdb), p2r_encoder.exe, ui\
                      :: P2Render\: the same without the pdb, plus README.txt and an empty data\
build.cmd -Run        :: and start P2Render\P2Render.exe
build.cmd -CleanGame  :: remove the old d3d9.dll proxy (first versions) from <Portal 2>\bin
```

A plain batch script (no PowerShell policy to fight, no module to install). It activates the
toolchain twice at most, once per architecture: calling `vcvars*.bat` repeatedly in the same `cmd`
session kills the script.

P2Render.exe must not be running when building (its files are locked).

**End-to-end test without the window** (`--no-window`: server only, the URL with its token is the
first line of `devBuild\p2render.log`; API calls need header `X-Token: <token>`):

```powershell
Start-Process devBuild\P2Render.exe --no-window; Start-Sleep 2
$u = (Get-Content devBuild\p2render.log -First 1) -replace '.*ui at ',''; $t = ($u -split 't=')[1]; $b = ($u -split '/\?')[0]; $h = @{"X-Token"=$t}
Invoke-RestMethod -Method Post "$b/api/add-paths" -Headers $h -Body '{"paths":["E:\\...\\portal2\\p2r_ref.dem"]}'
Invoke-RestMethod -Method Post "$b/api/start" -Headers $h -Body '{}'
Invoke-RestMethod "$b/api/state" -Headers $h   # poll until running == false
```

Open the same URL in a browser tab to see / click the interface.
`portal2\p2r_ref.dem` is a 12 s test demo recorded with the current build. Expect it `done` with 730
frames in ~1.3 s, the MP4 in `videos_out` next to the executable (the setting can point anywhere),
nothing left in `portal2\` (temp demo + WAV deleted), and the game closed. An old demo (e.g.
`a2_228.dem`) must fail fast with "the demo did not start". Check a video with
`node tools/preview/serve.js` (copy the mp4 to `tools/preview/test.mp4`, then http://localhost:8765).

API: `GET /api/state`; `POST /api/settings|add-dialog|add-paths|upload?name=|remove|clear|retry|start|cancel|browse-out|browse-game|open|visualise|preview|quit-game|bye`.
`/api/bye?t=<token>` is sent by the page on `pagehide` through `sendBeacon` (no header possible):
the app quits 2 s later, so closing the tab closes P2Render.
`POST /api/preview {id}` renders that demo **hidden** while ffmpeg writes an HLS stream into
`<data>\stream` (served at `GET /stream/*`, played with hls.js in the interface), then remuxes it
(`-c copy`) into the usual MP4 in the videos folder. The old "visible game window" mode is gone.
`POST /api/visualise {path,dir,out,ffArgs,ffmpegPath,width,height,fps}` is the streaming POC's
variant: it brings its own ffmpeg arguments and produces no final file. Used by the POC below.

POC « visualisation en direct » (hors dépôt) : page + petit serveur node qui pilote P2Render.exe
(`--no-window`) et sert le flux qui grossit (HLS via hls.js ou MP4 fragmenté dans `<video>`) :
`node server.js`, puis http://127.0.0.1:8787.

## How it works

1. **Launch.** The app runs `p2r_launch.exe "<game>" "<exe dir>\p2r_core.dll" -game portal2 -novid -sw
   -noborder -w W -h H`. The core is loaded before main(); it waits for shaderapidx9.dll and redirects
   its `Direct3DCreate9` import to `Direct3DCreate9Ex`. **Do not inline-hook d3d9!Direct3DCreate9
   instead**: the Steam overlay hooks it and CreateDevice, gets the Ex device, and the game crashed
   (ntdll AV) right after its first Present. Through the IAT, as with the first d3d9.dll proxy, the
   overlay never sees the creation.
2. **D3D9Ex.** d3d9ex.cpp patches the IDirect3D9Ex / device vtables (MANAGED -> DEFAULT(+DYNAMIC),
   CreateDeviceEx, small DXT mip locks without a rect...). See re/notes.md.
3. **Engine.** The first Present hooks the client's FrameStageNotify; on the first FRAME_START the core
   hooks `CVideoMode::WriteMovieFrame` (slot 23, found by byte pattern) and creates the control block
   (`core_state` READY).
4. **Job.** The app copies the demo to `portal2\p2render_tmp\demo<id>.dem`, writes a P2RJob and bumps
   `cmd_seq`. The core sets `mat_queue_mode 0; fps_max 0; host_framerate <fps>`, runs `playdemo`,
   and once the demo is in game runs `startmovie p2r_tmp_<id> wav`: the engine paces frames and writes
   the WAV; our WriteMovieFrame replacement StretchRects the back buffer into one of 8 shared
   A8R8G8B8 textures (+ EVENT query), published to the encoder by semaphore. Demo over (not playing for
   10 frames) -> `endmovie`, cvars restored, `disconnect`, encoder told the frame count. The encoder
   finalizes, muxes the WAV (AAC 192k) into `<out>\<demo name>.mp4` and deletes the WAV.
5. **Lifetime.** The page closes: `pagehide` sends `/api/bye`, the app quits 2 s later; a running
   render is cancelled and the game closed. A poll timeout (2 min, 10 min headless) covers a crashed
   browser.

## Status (2026-10-20)

Working end to end from the interface: launch, render, encode with sound, open/show the video,
several demos per game session, incompatible demos reported. 720p ~570 fps, 1080p ~540 fps
(`startmovie` TGA ~126 fps). Not tested yet: long demos / demos with level changes, co-op demos,
resolutions above the desktop size, AMD/Intel encoders, Steam not running.

Ideas: preview thumbnails of demos, per-demo output names, a "keep game open" session with a live
preview, CQP/quality modes, a texture pool in the encoder.
