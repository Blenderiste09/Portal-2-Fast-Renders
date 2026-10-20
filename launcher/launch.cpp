// p2r_launch.exe (x86): starts portal2.exe with p2r_core.dll loaded before the game's main(), so the
// D3D9 device can be created as D3D9Ex. Used by P2Render.exe; nothing is copied into the game.
//
//   p2r_launch.exe "<game folder>" "<p2r_core.dll path>" [portal2 arguments...]
//   exit code 0 and "pid <n>" on stdout on success.
//
// How: portal2.exe is created suspended and its entry point is patched with "jmp $". Once the main
// thread spins there, process initialisation is complete (kernel32 loaded, loader lock free) but
// main() has not loaded launcher.dll yet. The DLL is loaded with a remote LoadLibraryW, the entry
// bytes are restored and the game runs. (Same technique as ChocolateLoading's chocoload.exe.)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

// LoadLibraryW(dll) in the child through a remote thread. kernel32 is mapped at the same address in
// every 32-bit process of a session, so our LoadLibraryW address is valid in the child.
static bool Inject(HANDLE proc, const wchar_t* dll)
{
    size_t n = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    bool ok = false;
    if (WriteProcessMemory(proc, remote, dll, n, nullptr)) {
        FARPROC load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE th = CreateRemoteThread(proc, nullptr, 0, (LPTHREAD_START_ROUTINE)load, remote, 0, nullptr);
        if (th) {
            DWORD code = 0;
            ok = WaitForSingleObject(th, 30000) == WAIT_OBJECT_0 && GetExitCodeThread(th, &code) && code;
            CloseHandle(th);
        }
    }
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return ok;
}

// Address of the EXE entry point of a freshly created (suspended) process: EBX holds its PEB.
static uintptr_t EntryPoint(HANDLE proc, HANDLE thread)
{
    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_FULL;
    uintptr_t image = 0;
    if (!GetThreadContext(thread, &ctx) ||
        !ReadProcessMemory(proc, (void*)(uintptr_t)(ctx.Ebx + 8), &image, sizeof(image), nullptr))
        return 0;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS nt;
    if (!ReadProcessMemory(proc, (void*)image, &dos, sizeof(dos), nullptr) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        !ReadProcessMemory(proc, (void*)(image + dos.e_lfanew), &nt, sizeof(nt), nullptr) || nt.Signature != IMAGE_NT_SIGNATURE)
        return 0;
    return image + nt.OptionalHeader.AddressOfEntryPoint;
}

static bool WriteCode(HANDLE proc, uintptr_t addr, const void* bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtectEx(proc, (void*)addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = WriteProcessMemory(proc, (void*)addr, bytes, n, nullptr) != 0;
    VirtualProtectEx(proc, (void*)addr, n, old, &old);
    FlushInstructionCache(proc, (void*)addr, n);
    return ok;
}

static bool LoadAtEntry(const PROCESS_INFORMATION& pi, const wchar_t* dll)
{
    uintptr_t entry = EntryPoint(pi.hProcess, pi.hThread);
    unsigned char original[2], spin[2] = {0xEB, 0xFE}; // jmp $
    if (!entry || !ReadProcessMemory(pi.hProcess, (void*)entry, original, 2, nullptr) || !WriteCode(pi.hProcess, entry, spin, 2)) {
        fprintf(stderr, "could not patch the entry point\n");
        return false;
    }
    ResumeThread(pi.hThread);
    bool parked = false;
    for (int waited = 0; waited < 30000 && !parked; waited += 5) {
        Sleep(5);
        SuspendThread(pi.hThread);
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL;
        parked = GetThreadContext(pi.hThread, &c) && c.Eip == (DWORD)entry;
        if (!parked) ResumeThread(pi.hThread);
    }
    if (!parked) {
        fprintf(stderr, "the game never reached its entry point\n");
        return false;
    }
    bool ok = Inject(pi.hProcess, dll);
    if (!ok) fprintf(stderr, "LoadLibrary of p2r_core.dll failed in the game\n");
    if (!WriteCode(pi.hProcess, entry, original, 2)) ok = false;
    return ok;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: p2r_launch <game folder> <p2r_core.dll> [arguments]\n");
        return 2;
    }
    const wchar_t* game = argv[1];
    const wchar_t* dll = argv[2];
    wchar_t exe[MAX_PATH];
    swprintf_s(exe, L"%s\\portal2.exe", game);
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "portal2.exe or p2r_core.dll not found\n");
        return 3;
    }
    wchar_t cmd[8192];
    swprintf_s(cmd, L"\"%s\"", exe);
    for (int i = 3; i < argc; ++i) {
        wcscat_s(cmd, L" ");
        bool quote = wcschr(argv[i], L' ') != nullptr;
        if (quote) wcscat_s(cmd, L"\"");
        wcscat_s(cmd, argv[i]);
        if (quote) wcscat_s(cmd, L"\"");
    }
    STARTUPINFOW si = {sizeof(si)};
    // P2R_HIDDEN=1 (P2Render invisible rendering): the game's first ShowWindow keeps it hidden; the
    // core hides it again whenever the game shows it (same variable, inherited).
    wchar_t hidden[8] = {};
    if (GetEnvironmentVariableW(L"P2R_HIDDEN", hidden, 8) && hidden[0] == L'1') {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, game, &si, &pi)) {
        fprintf(stderr, "CreateProcess failed (%lu)\n", GetLastError());
        return 4;
    }
    bool ok = LoadAtEntry(pi, dll);
    if (ok) {
        ResumeThread(pi.hThread);
        printf("pid %lu\n", pi.dwProcessId);
    } else {
        TerminateProcess(pi.hProcess, 1); // never leave a half-started game behind
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok ? 0 : 5;
}
