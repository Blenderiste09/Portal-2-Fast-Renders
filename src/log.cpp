#include "log.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

namespace xlog {

static HANDLE g_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;
static DWORD g_start;

void Init(void* module)
{
    InitializeCriticalSection(&g_lock);
    g_start = GetTickCount();
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA((HMODULE)module, path, MAX_PATH);
    while (n > 0 && path[n - 1] != '\\') --n;
    strcpy_s(path + n, MAX_PATH - n, "p2r_core.log");
    g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Write(const char* fmt, ...)
{
    if (g_file == INVALID_HANDLE_VALUE) return;
    char buf[1024];
    DWORD t = GetTickCount() - g_start;
    int n = sprintf_s(buf, "[%6lu.%03lu] ", t / 1000, t % 1000);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) m = (int)strlen(buf + n);
    n += m;
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&g_lock);
    DWORD w;
    WriteFile(g_file, buf, n, &w, nullptr);
    LeaveCriticalSection(&g_lock);
}

} // namespace xlog
