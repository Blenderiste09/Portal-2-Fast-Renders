// Runs the game's plain Direct3D 9 renderer on a Direct3D 9Ex device.
//
// D3D9Ex differs from D3D9 in what the shader API relies on:
//  - CreateDevice must become CreateDeviceEx (with a D3DDISPLAYMODEEX in exclusive fullscreen);
//  - D3DPOOL_MANAGED does not exist: managed textures become D3DPOOL_DEFAULT + D3DUSAGE_DYNAMIC
//    (the shader API locks them to upload texels), managed buffers become D3DPOOL_DEFAULT;
//  - the device is not "lost" any more (TestCooperativeLevel returns S_OK).
// All hooks patch the vtables of the real d3d9 objects, so overlays and tools still see real
// d3d9 interfaces.
#pragma once

struct IDirect3D9Ex;
struct IDirect3DDevice9Ex;
struct HWND__;

namespace d3d9ex {
void Init();                    // reads the command line (-nod3d9ex disables everything)
bool Enabled();
void HookD3D9Ex(IDirect3D9Ex* d3d);
IDirect3DDevice9Ex* Device();   // the game's device once created (null before)
::HWND__* GameWindow();         // its focus window
}
