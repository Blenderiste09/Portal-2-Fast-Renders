@echo off
rem Builds P2Render into dist\ with the Visual Studio 18 toolchain:
rem   P2Render.exe     (x64)  the application (local web interface in the default browser)
rem   p2r_launch.exe   (x86)  starts portal2.exe with p2r_core.dll loaded before main()
rem   p2r_core.dll     (x86)  injected into the game: D3D9Ex + GPU capture + job control
rem   p2r_encoder.exe  (x64)  D3D11 + Media Foundation hardware encoder, WAV mux
rem   ui\                     the interface (index.html, icon.svg)
rem Nothing is installed in the game folder. P2Render.exe must not be running: its files are locked.
rem
rem   build.cmd                 build dist\ and dist\portable\
rem   build.cmd -Run            build and start dist\portable\P2Render.exe
rem   build.cmd -CleanGame      remove files older versions deployed into <Portal 2>\bin (d3d9.dll proxy)
rem   build.cmd -GameDir "..."  Portal 2 folder used by -CleanGame
rem
rem The toolchain environment is activated twice at most (once per architecture): calling
rem vcvars*.bat repeatedly in the same cmd session ends up killing the script.
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"
set "DIST=%ROOT%\dist"
set "GAMEDIR=E:\Program Files (x86)\Steam\steamapps\common\Portal 2"
set "DO_RUN=0"
set "DO_CLEAN=0"

:parse
if "%~1"=="" goto parsed
if /I "%~1"=="-Run" set "DO_RUN=1" & shift & goto parse
if /I "%~1"=="-CleanGame" set "DO_CLEAN=1" & shift & goto parse
if /I "%~1"=="-GameDir" goto arg_gamedir
echo Unknown argument: %~1
exit /b 2

:arg_gamedir
shift
set "GAMEDIR=%~1"
shift
goto parse

:parsed
if "%DO_CLEAN%"=="1" goto clean
if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%DIST%" mkdir "%DIST%"

rem --- toolchain ---------------------------------------------------------------------------
set "VCBUILD=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build"
if exist "%VCBUILD%\vcvars64.bat" goto vs_found
set "PF86=%ProgramFiles(x86)%"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vs
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VCBUILD=%%i\VC\Auxiliary\Build"
if not exist "%VCBUILD%\vcvars64.bat" goto no_vs
:vs_found

rem /MT: the tools depend on no runtime redistributable, they run on a bare Windows.
set "COMMON=/nologo /O2 /MT /EHsc /W3 /Zi /std:c++17 /utf-8 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS"

rem --- x64: icon + version information, P2Render.exe, p2r_encoder.exe -----------------------
call "%VCBUILD%\vcvars64.bat" >nul 2>&1
if errorlevel 1 goto no_vs

rem Icon and version information of the application (taskbar, file properties). rc.exe needs
rem the backslashes of the icon path doubled.
set "ICON=%ROOT%\app\ui\icon.ico"
set "ICON=%ICON:\=\\%"
set "APP_RC=%BUILD%\app.rc"
> "%APP_RC%" echo #include ^<windows.h^>
>> "%APP_RC%" echo 1 ICON "%ICON%"
>> "%APP_RC%" echo 1 VERSIONINFO
>> "%APP_RC%" echo FILEVERSION 1,0,0,0
>> "%APP_RC%" echo PRODUCTVERSION 1,0,0,0
>> "%APP_RC%" echo FILEFLAGSMASK 0x3fL
>> "%APP_RC%" echo FILEFLAGS 0x0L
>> "%APP_RC%" echo FILEOS 0x40004L
>> "%APP_RC%" echo FILETYPE 0x1L
>> "%APP_RC%" echo FILESUBTYPE 0x0L
>> "%APP_RC%" echo BEGIN
>> "%APP_RC%" echo     BLOCK "StringFileInfo"
>> "%APP_RC%" echo     BEGIN
>> "%APP_RC%" echo         BLOCK "040904b0"
>> "%APP_RC%" echo         BEGIN
>> "%APP_RC%" echo             VALUE "FileDescription", "P2Render - rendu rapide de demos Portal 2"
>> "%APP_RC%" echo             VALUE "FileVersion", "1.0"
>> "%APP_RC%" echo             VALUE "InternalName", "P2Render"
>> "%APP_RC%" echo             VALUE "OriginalFilename", "P2Render.exe"
>> "%APP_RC%" echo             VALUE "ProductName", "P2Render"
>> "%APP_RC%" echo             VALUE "ProductVersion", "1.0"
>> "%APP_RC%" echo         END
>> "%APP_RC%" echo     END
>> "%APP_RC%" echo     BLOCK "VarFileInfo"
>> "%APP_RC%" echo     BEGIN
>> "%APP_RC%" echo         VALUE "Translation", 0x409, 1200
>> "%APP_RC%" echo     END
>> "%APP_RC%" echo END
rc.exe /nologo /fo "%BUILD%\app.res" "%APP_RC%" > "%BUILD%\rc.log" 2>&1
if errorlevel 1 ( type "%BUILD%\rc.log" & goto fail )

set "TAG=app"
set "OUT=%BUILD%\app"
set "ARGS=%COMMON% /DUNICODE /D_UNICODE "%ROOT%\app\main.cpp" "%BUILD%\app.res" /Fe:P2Render.exe /link /DEBUG /SUBSYSTEM:WINDOWS ws2_32.lib shell32.lib ole32.lib bcrypt.lib advapi32.lib user32.lib urlmon.lib"
call :compile
if errorlevel 1 goto fail

set "TAG=encoder"
set "OUT=%BUILD%\encoder"
set "ARGS=%COMMON% /DUNICODE /D_UNICODE "%ROOT%\encoder\encoder.cpp" /Fe:p2r_encoder.exe /link /DEBUG d3d11.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib"
call :compile
if errorlevel 1 goto fail

rem --- x86: p2r_core.dll (every src\*.cpp, through a response file) and p2r_launch.exe ------
call "%VCBUILD%\vcvars32.bat" >nul 2>&1
if errorlevel 1 goto no_vs

if not exist "%BUILD%\core" mkdir "%BUILD%\core"
> "%BUILD%\core\sources.rsp" ( for %%f in ("!ROOT!\src\*.cpp") do echo "%%~ff" )
set "TAG=core"
set "OUT=%BUILD%\core"
set "ARGS=%COMMON% /LD @sources.rsp /Fe:p2r_core.dll /link /DEBUG /OPT:REF /OPT:ICF kernel32.lib user32.lib psapi.lib"
call :compile
if errorlevel 1 goto fail

set "TAG=launch"
set "OUT=%BUILD%\launch"
set "ARGS=%COMMON% /DUNICODE /D_UNICODE "%ROOT%\launcher\launch.cpp" /Fe:p2r_launch.exe /link /DEBUG"
call :compile
if errorlevel 1 goto fail

rem --- dist\ ---------------------------------------------------------------------------------
if not exist "%DIST%\ui" mkdir "%DIST%\ui"
copy /y "%BUILD%\core\p2r_core.dll" "%DIST%" >nul || goto copy_failed
copy /y "%BUILD%\core\p2r_core.pdb" "%DIST%" >nul
copy /y "%BUILD%\launch\p2r_launch.exe" "%DIST%" >nul || goto copy_failed
copy /y "%BUILD%\encoder\p2r_encoder.exe" "%DIST%" >nul || goto copy_failed
copy /y "%BUILD%\app\P2Render.exe" "%DIST%" >nul || goto copy_failed
copy /y "%ROOT%\app\ui\*" "%DIST%\ui" >nul || goto copy_failed

rem --- portable folder ---------------------------------------------------------------------
rem dist\portable: everything needed, data included (settings, imported demos and downloaded
rem ffmpeg all live in portable\data), so the folder can be copied or zipped as is. No installer,
rem no registry, nothing in %LOCALAPPDATA%, nothing in the game folder.
set "PORTABLE=%DIST%\portable"
if not exist "%PORTABLE%\ui" mkdir "%PORTABLE%\ui"
if not exist "%PORTABLE%\data" mkdir "%PORTABLE%\data"
copy /y "%DIST%\P2Render.exe" "%PORTABLE%" >nul || goto copy_failed
copy /y "%DIST%\p2r_launch.exe" "%PORTABLE%" >nul || goto copy_failed
copy /y "%DIST%\p2r_core.dll" "%PORTABLE%" >nul || goto copy_failed
copy /y "%DIST%\p2r_encoder.exe" "%PORTABLE%" >nul || goto copy_failed
copy /y "%DIST%\ui\*" "%PORTABLE%\ui" >nul || goto copy_failed
if exist "%PORTABLE%\LISEZ-MOI.txt" del /f /q "%PORTABLE%\LISEZ-MOI.txt"

set "PORT_README=%PORTABLE%\README.txt"
> "%PORT_README%" echo P2Render - fast rendering of Portal 2 demos
>> "%PORT_README%" echo.
>> "%PORT_README%" echo 1. Double-click P2Render.exe
>> "%PORT_README%" echo 2. Drop a demo ^(.dem^) into the window
>> "%PORT_README%" echo 3. Click Start rendering ^(the videos land in the videos_out folder next to this one^)
>> "%PORT_README%" echo.
>> "%PORT_README%" echo Portable build: nothing is installed. Everything the application writes stays in this folder
>> "%PORT_README%" echo ^(the data subfolder: settings, imported demos, ffmpeg downloaded if needed^).
>> "%PORT_README%" echo Move or copy the whole folder, it works as is.
>> "%PORT_README%" echo.
>> "%PORT_README%" echo Nothing is installed inside Portal 2: the application starts the game and injects its module
>> "%PORT_README%" echo for the time of the render only.
>> "%PORT_README%" echo.
>> "%PORT_README%" echo If Portal 2 is not found ^(Steam installation elsewhere^), set its folder in the settings.

echo built %DIST%
echo portable: %PORTABLE% (copy or zip the whole folder)
if "%DO_RUN%"=="1" start "" "%PORTABLE%\P2Render.exe"
exit /b 0

:clean
rem No parenthesised block here: the default game folder contains "(x86)", which would close it.
for %%f in (d3d9.dll d3d9.pdb p2r_encoder.exe p2r_encoder.log) do call :clean_one "%%f"
exit /b 0

:clean_one
set "F=%GAMEDIR%\bin\%~1"
if not exist "%F%" exit /b 0
del /f /q "%F%"
echo removed %F%
exit /b 0

rem --- helpers ------------------------------------------------------------------------------

rem Compiles ARGS into OUT with the toolchain already activated by the caller. ARGS is expanded
rem here so quoted paths survive; the log is filtered to errors and warnings.
:compile
if not exist "%OUT%" mkdir "%OUT%"
pushd "%OUT%"
cl.exe %ARGS% > cl.log 2>&1
set "RC=!ERRORLEVEL!"
popd
findstr /I /C:" error" /C:" warning" "%OUT%\cl.log"
if not "!RC!"=="0" (
    echo BUILD FAILED: !TAG! ^(see "%OUT%\cl.log"^)
    exit /b 1
)
echo built !TAG!
exit /b 0

:no_vs
echo error: Visual Studio 18 C++ tools not found ^(vswhere or VC\Auxiliary\Build^)
exit /b 1

:copy_failed
echo error: copy into dist failed - is P2Render.exe still running?
exit /b 1

:fail
exit /b 1
