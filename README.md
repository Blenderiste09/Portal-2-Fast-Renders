# P2Render: Ultra-Fast Portal 2 Demo Rendering

**I wanted to use the technology SourceDemoRender (https://github.com/crashfort/SourceDemoRender) uses to render demos super fast in other Source engine games. The problem: those games use D3D9Ex, which supports GPU texture sharing, and Portal 2 uses D3D9, which doesn't.
This project is a proof of concept. It redirects the game's Direct3DCreate9 import to Direct3DCreate9Ex and adapts the CreateDevice call to CreateDeviceEx, without touching the game folder. That gives access to texture sharing, making the rendered frames accessible from an external process.**

P2Render is a standalone application that converts Portal 2 demos into MP4 videos (H.264 or HEVC, with audio) much faster than `startmovie`. Frames never pass through the CPU: the game runs in Direct3D 9Ex, each frame is copied to a shared texture on the GPU, then encoded by the GPU's hardware encoder (NVENC / AMF / QuickSync).

Nothing is installed in the game directory, and there are no console commands to type.

## Usage

1. Launch `P2Render.exe`: the interface opens in your default browser. Steam must be running, and Portal 2 must be closed.
2. Add demos: click **Add Demos**, or drag and drop `.dem` files into the list.
3. Choose the resolution, frame rate, format, quality, and output folder for the videos.
4. Click **Start Rendering**.

Alternatively, you can click on the camera icon next to a demo to see the video **while** it's rendered. You have to enable advanced settings and download ffmpeg to use this feature.

P2Render launches Portal 2 and renders the demos one after another (with progress, speed, and estimated time remaining displayed), then closes the game if the corresponding option is checked. Once a demo is finished, its buttons let you play the video or open its containing folder.

## Speed

Measurements on a Ryzen 7 5700X with an NVIDIA GPU, using a 12.2-second demo at 60 FPS:

| Method | Speed |
|---|---|
| In-game `startmovie` (TGA frames, requiring subsequent encoding) | ~126 FPS |
| P2Render at 720p | ~570 FPS (9x real-time) |
| P2Render at 1080p | ~540 FPS |

Including the time needed to launch Portal 2, two demos are rendered in approximately 12 seconds.

## Building

Visual Studio 2026 with the C++ toolsets (x86 and x64) is required.

```bat
build.cmd          :: builds devBuild\ (development) and P2Render\ (the folder to hand out)
build.cmd -Run     :: builds and then launches P2Render\P2Render.exe
```

`build.cmd` is a plain batch script: nothing to install.

The `P2Render\` folder is the portalable version: everything fits inside it (application, injected
module, encoder, interface, README) and the application only writes into `P2Render\data` (settings,
imported demos, downloaded ffmpeg). Copy or zip the whole folder (`tar -a -c -f P2Render.zip
P2Render`): it works as is, with no installation and nothing left anywhere else. `devBuild\` (with
the debug symbols) is what you run while working; `build\` only holds compiler work files.

## Logs for Troubleshooting

The logs are located in the P2Render directory:
- `p2render.log` (application);
- `p2r_core.log` (game-side component);
- `p2r_encoder.log` (encoder).

Demos recorded before the game's 2026 update can no longer be played back (the game reports a `DataTable` error). P2Render flags them as incompatible. Might work on compatibility later, this is a proof of concept project
