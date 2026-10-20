// The few engine pieces the renderer needs (Portal 2 Windows build, see re/notes.md):
// console variables (ICvar), IVEngineClient, the engine's movie recorder (startmovie), and a
// per-frame callback on the main thread (IBaseClientDLL::FrameStageNotify).
#pragma once
#include <stdint.h>

struct CCommand {
    enum { COMMAND_MAX_ARGC = 64, COMMAND_MAX_LENGTH = 512 };
    int m_nArgc;
    int m_nArgv0Size;
    char m_pArgSBuffer[COMMAND_MAX_LENGTH];
    char m_pArgvBuffer[COMMAND_MAX_LENGTH];
    const char* m_ppArgv[COMMAND_MAX_ARGC];

    int ArgC() const { return m_nArgc; }
    const char* Arg(int i) const { return (i >= 0 && i < m_nArgc) ? m_ppArgv[i] : ""; }
};
typedef void (*CommandCallback)(const CCommand& args);

namespace engine {

// Called from Present until it returns true: hooks the client's FrameStageNotify once client.dll
// is loaded. Everything else is set up on the main thread on the first frame.
bool TryHookFrame();

// Main-thread services (valid after the first frame).
void Msg(const char* fmt, ...);              // console (tier0 ConMsg) + log
void Warn(const char* fmt, ...);
void Run(const char* cmd);                   // ClientCmd_Unrestricted (runs later in the frame)
const char* GetCvarString(const char* name); // null if unknown
bool IsPlayingDemo();
bool IsInGame();                             // fully signed on
bool IsLoading();                            // loading screen shown
const char* GameDir();                       // <Portal 2>\portal2 (no trailing slash)

// Engine movie frames: when set, the callback replaces CVideoMode::WriteMovieFrame (which reads
// the back buffer to the CPU even for "startmovie x wav"). Return value: frame consumed.
typedef bool (*MovieFrameCallback)();
void SetMovieFrameCallback(MovieFrameCallback cb);
bool MovieHookReady();

// Main-thread per-frame callback (FRAME_START).
typedef void (*FrameCallback)();
void SetFrameCallback(FrameCallback cb);
// Called once on the main thread when the engine interfaces are available.
void SetInitCallback(FrameCallback cb);

} // namespace engine
