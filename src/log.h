// Tiny thread-safe logger: <Portal 2>\d3d9ex.log (next to portal2.exe).
#pragma once

namespace xlog {
void Init();
void Write(const char* fmt, ...);
}

#define LOG(...) ::xlog::Write(__VA_ARGS__)
