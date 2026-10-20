// Tiny thread-safe logger: p2r_core.log next to the given module (the P2Render folder).
#pragma once

namespace xlog {
void Init(void* module);
void Write(const char* fmt, ...);
}

#define LOG(...) ::xlog::Write(__VA_ARGS__)
