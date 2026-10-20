// Demo rendering: console commands, GPU copy of every engine movie frame into shared D3D9Ex
// textures, and the p2r_encoder.exe process that encodes them (see shared.h for the protocol).
#pragma once
#include <windows.h>

namespace capture {
void Init(HMODULE self);     // DllMain: where p2r_encoder.exe lives (next to d3d9.dll)
void OnPresentHooked();      // first Present: hook the engine (main thread setup)
bool SkipPresent();          // true while rendering with window=0
}
