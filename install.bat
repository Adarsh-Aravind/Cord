@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM ===================================================================
REM  Cord - plugin installer for OBS Studio and Streamlabs Desktop
REM
REM  Double-click to install into every app it finds, or run from a
REM  prompt with:
REM
REM    install.bat                 install into OBS and Streamlabs
REM    install.bat /obs            OBS Studio only
REM    install.bat /slobs          Streamlabs Desktop only
REM    install.bat /dir "C:\..."   a specific OBS install directory
REM    install.bat /uninstall      remove the plugin again
REM    install.bat /force          do not stop if OBS is running
REM
REM  Run it from the repository root after a build, or from an
REM  unpacked release zip. It re-runs itself as administrator if the
REM  target directory needs it.
REM ===================================================================

set "PLUGIN=cord"
set "PRETTY=Cord"

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "DO_OBS="
set "DO_SLOBS="
set "UNINSTALL="
set "EXPLICIT_DIR="
set "ELEVATED="
set "FORCE="

REM --- arguments ------------------------------------------------------

:args
if "%~1"=="" goto args_done
if /i "%~1"=="/obs"       (set "DO_OBS=1"   & shift & goto args)
if /i "%~1"=="/slobs"     (set "DO_SLOBS=1" & shift & goto args)
if /i "%~1"=="/streamlabs" (set "DO_SLOBS=1" & shift & goto args)
if /i "%~1"=="/all"       (set "DO_OBS=1"   & set "DO_SLOBS=1" & shift & goto args)
if /i "%~1"=="/uninstall" (set "UNINSTALL=1" & shift & goto args)
if /i "%~1"=="/remove"    (set "UNINSTALL=1" & shift & goto args)
if /i "%~1"=="/dir"       (set "EXPLICIT_DIR=%~2" & shift & shift & goto args)
if /i "%~1"=="/elevated"  (set "ELEVATED=1" & shift & goto args)
if /i "%~1"=="/force"     (set "FORCE=1" & shift & goto args)
if /i "%~1"=="/?"         goto usage
if /i "%~1"=="-h"         goto usage
if /i "%~1"=="--help"     goto usage
echo [x] Unknown option: %~1
echo.
goto usage

:args_done
if not defined DO_OBS if not defined DO_SLOBS (
  set "DO_OBS=1"
  set "DO_SLOBS=1"
)

echo.
echo   %PRETTY% installer
echo   ---------------------------------------------------------------
echo.

REM --- find what we are installing -----------------------------------

if defined UNINSTALL goto find_targets

set "SRC_DLL="
for %%C in (
  "build\RelWithDebInfo\%PLUGIN%.dll"
  "build\Release\%PLUGIN%.dll"
  "build\Debug\%PLUGIN%.dll"
  "build\%PLUGIN%.dll"
  "obs-plugins\64bit\%PLUGIN%.dll"
  "bin\64bit\%PLUGIN%.dll"
  "%PLUGIN%.dll"
) do if not defined SRC_DLL if exist "%ROOT%\%%~C" set "SRC_DLL=%ROOT%\%%~C"

if not defined SRC_DLL (
  echo   [x] %PLUGIN%.dll not found.
  echo.
  echo       Looked under "%ROOT%\build". Build it first:
  echo.
  echo         cmake --preset windows-x64
  echo         cmake --build --preset windows-x64
  echo.
  echo       See docs\building.md.
  goto fail
)

set "SRC_PDB="
if exist "%SRC_DLL:.dll=.pdb%" set "SRC_PDB=%SRC_DLL:.dll=.pdb%"

set "SRC_LOCALE="
for %%C in ("data\locale" "data\obs-plugins\%PLUGIN%\locale") do (
  if not defined SRC_LOCALE if exist "%ROOT%\%%~C\en-US.ini" set "SRC_LOCALE=%ROOT%\%%~C"
)
if not defined SRC_LOCALE (
  echo   [x] data\locale\en-US.ini not found next to this script.
  goto fail
)

set "SRC_BIN="
for %%C in ("data\bin" "data\obs-plugins\%PLUGIN%\bin") do (
  if not defined SRC_BIN if exist "%ROOT%\%%~C" set "SRC_BIN=%ROOT%\%%~C"
)

set "DEPS_OK=1"
if not defined SRC_BIN set "DEPS_OK="
if defined SRC_BIN if not exist "%SRC_BIN%\adb.exe" set "DEPS_OK="
if defined SRC_BIN if not exist "%SRC_BIN%\scrcpy-server" set "DEPS_OK="

echo   plugin:   %SRC_DLL%
echo   locale:   %SRC_LOCALE%
if defined SRC_BIN echo   runtime:  %SRC_BIN%
echo.

if not defined DEPS_OK (
  echo   [x] adb.exe and/or scrcpy-server are missing from data\bin.
  echo       The plugin will load but will not find a phone. Fetch them with:
  echo.
  echo         powershell -ExecutionPolicy Bypass -File scripts\fetch-deps.ps1 -SkipBuildDeps
  echo.
  choice /c yn /n /m "   Install anyway? [y/n] "
  if errorlevel 2 goto cancelled
  echo.
)

REM --- find the targets ----------------------------------------------

:find_targets
set "N=0"

if defined EXPLICIT_DIR (
  call :add_obs_target "%EXPLICIT_DIR%" "the directory you gave"
  if not defined TARGET_1 (
    echo   [x] "%EXPLICIT_DIR%" does not look like an OBS install directory
    echo       ^(no bin\64bit\obs64.exe, no obs-plugins\64bit^).
    goto fail
  )
  goto targets_done
)

if defined DO_OBS call :find_obs
if defined DO_SLOBS call :find_slobs

:targets_done
if "%N%"=="0" (
  if defined DO_OBS if defined DO_SLOBS echo   [x] Found neither OBS Studio nor Streamlabs Desktop.
  if defined DO_OBS if not defined DO_SLOBS echo   [x] No OBS Studio install found.
  if defined DO_SLOBS if not defined DO_OBS echo   [x] No Streamlabs Desktop install found.
  echo.
  echo       If OBS is installed somewhere unusual, point at it:
  echo         install.bat /dir "D:\path\to\OBS Studio"
  goto fail
)

echo   Targets:
for /l %%I in (1,1,%N%) do echo     - !LABEL_%%I!  ^> !TARGET_%%I!
echo.

REM --- OBS must not be running (the DLL would be locked) -------------

if defined FORCE goto skip_running_check
call :check_running obs64.exe "OBS Studio"
if errorlevel 1 goto fail
call :check_running "Streamlabs OBS.exe" "Streamlabs Desktop"
if errorlevel 1 goto fail
:skip_running_check

REM --- elevate if any target is not writable -------------------------

set "NEED_ADMIN="
for /l %%I in (1,1,%N%) do (
  set "T=!TARGET_%%I!"
  2>nul ( >>"!T!\.cord-write-test" echo ok ) && (del "!T!\.cord-write-test" >nul 2>&1) || set "NEED_ADMIN=1"
)

if defined NEED_ADMIN if not defined ELEVATED (
  echo   Needs administrator rights to write there - asking for them...
  echo.
  set "RELAUNCH=/elevated"
  if defined DO_OBS       set "RELAUNCH=!RELAUNCH! /obs"
  if defined DO_SLOBS     set "RELAUNCH=!RELAUNCH! /slobs"
  if defined UNINSTALL    set "RELAUNCH=!RELAUNCH! /uninstall"
  if defined FORCE        set "RELAUNCH=!RELAUNCH! /force"
  if defined EXPLICIT_DIR set "RELAUNCH=!RELAUNCH! /dir \"%EXPLICIT_DIR%\""
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '!RELAUNCH!' -Verb RunAs" >nul 2>&1
  if errorlevel 1 (
    echo   [x] Elevation was refused. Right-click install.bat and pick
    echo       "Run as administrator", or install into a directory you own.
    goto fail
  )
  endlocal
  exit /b 0
)

if defined NEED_ADMIN if defined ELEVATED (
  echo   [x] Still cannot write to the target directory even as administrator.
  goto fail
)

REM --- do it ----------------------------------------------------------

set "FAILED="
for /l %%I in (1,1,%N%) do (
  echo   !LABEL_%%I!:
  if defined UNINSTALL ( call :remove_one "!TARGET_%%I!" ) else ( call :install_one "!TARGET_%%I!" )
  if errorlevel 1 set "FAILED=1"
  echo.
)

if defined FAILED goto fail

if defined UNINSTALL (
  echo   Removed. Restart OBS / Streamlabs and the source will be gone.
) else (
  echo   Done. Restart OBS / Streamlabs, then add a source:
  echo.
  echo       +  ^>  Phone ^(%PRETTY%^)
  echo.
  echo   If it does not appear, check Help ^> Log Files ^> View Current Log
  echo   for a line reading: [%PLUGIN%] loaded version
)
echo.
goto done

REM ===================================================================
REM  subroutines
REM ===================================================================

:install_one
REM  %1 = app root; layout is OBS's native one:
REM         obs-plugins\64bit\cord.dll
REM         data\obs-plugins\cord\locale\*
REM         data\obs-plugins\cord\bin\*
set "T=%~1"
set "DATA=%T%\data\obs-plugins\%PLUGIN%"

if not exist "%T%\obs-plugins\64bit" mkdir "%T%\obs-plugins\64bit" 2>nul
if not exist "%DATA%\locale" mkdir "%DATA%\locale" 2>nul

copy /y "%SRC_DLL%" "%T%\obs-plugins\64bit\%PLUGIN%.dll" >nul
if errorlevel 1 (
  echo     [x] could not copy %PLUGIN%.dll
  exit /b 1
)
echo     obs-plugins\64bit\%PLUGIN%.dll

if defined SRC_PDB (
  copy /y "%SRC_PDB%" "%T%\obs-plugins\64bit\%PLUGIN%.pdb" >nul 2>&1
)

copy /y "%SRC_LOCALE%\*.ini" "%DATA%\locale\" >nul
if errorlevel 1 (
  echo     [x] could not copy the locale files
  exit /b 1
)
echo     data\obs-plugins\%PLUGIN%\locale\

if defined SRC_BIN (
  if not exist "%DATA%\bin" mkdir "%DATA%\bin" 2>nul
  xcopy /y /q /e /i "%SRC_BIN%\*" "%DATA%\bin\" >nul
  if errorlevel 1 (
    echo     [x] could not copy adb / scrcpy-server
    exit /b 1
  )
  echo     data\obs-plugins\%PLUGIN%\bin\
)
exit /b 0

:remove_one
set "T=%~1"
set "DATA=%T%\data\obs-plugins\%PLUGIN%"
set "GONE=1"

if exist "%T%\obs-plugins\64bit\%PLUGIN%.dll" (
  del /f /q "%T%\obs-plugins\64bit\%PLUGIN%.dll" >nul 2>&1
  if exist "%T%\obs-plugins\64bit\%PLUGIN%.dll" (
    echo     [x] could not delete %PLUGIN%.dll ^(is the app still running?^)
    exit /b 1
  )
  echo     removed obs-plugins\64bit\%PLUGIN%.dll
  set "GONE="
)
if exist "%T%\obs-plugins\64bit\%PLUGIN%.pdb" del /f /q "%T%\obs-plugins\64bit\%PLUGIN%.pdb" >nul 2>&1

if exist "%DATA%" (
  rmdir /s /q "%DATA%" >nul 2>&1
  if exist "%DATA%" (
    echo     [x] could not delete %DATA%
    exit /b 1
  )
  echo     removed data\obs-plugins\%PLUGIN%\
  set "GONE="
)

if defined GONE echo     nothing installed here
exit /b 0

:add_obs_target
REM  %1 = candidate app root, %2 = label. Accepted if it looks like OBS.
set "C=%~1"
if "%C%"=="" exit /b 0
if "%C:~-1%"=="\" set "C=%C:~0,-1%"
if not exist "%C%\bin\64bit\obs64.exe" if not exist "%C%\obs-plugins\64bit" exit /b 0
REM  do not add the same directory twice
for /l %%I in (1,1,%N%) do if /i "!TARGET_%%I!"=="%C%" exit /b 0
set /a N+=1
set "TARGET_%N%=%C%"
set "LABEL_%N%=%~2"
exit /b 0

:find_obs
REM  The installer records the install directory in the registry; Steam
REM  and winget builds both do. Fall back to the usual places.
for %%K in ("HKLM\SOFTWARE\OBS Studio" "HKLM\SOFTWARE\WOW6432Node\OBS Studio") do (
  for /f "tokens=2,*" %%a in ('reg query %%K /ve 2^>nul ^| findstr /i "REG_SZ"') do (
    call :add_obs_target "%%b" "OBS Studio"
  )
)
call :add_obs_target "%ProgramFiles%\obs-studio" "OBS Studio"
call :add_obs_target "%ProgramFiles(x86)%\obs-studio" "OBS Studio (32-bit install)"
call :add_obs_target "%ProgramW6432%\obs-studio" "OBS Studio"
exit /b 0

:find_slobs
REM  Streamlabs Desktop is Electron around libobs: its plugin directory
REM  lives inside the bundled obs-studio-node module.
set "SLSUB=resources\app.asar.unpacked\node_modules\obs-studio-node"
for %%R in (
  "%LOCALAPPDATA%\Programs\Streamlabs OBS"
  "%LOCALAPPDATA%\Programs\Streamlabs Desktop"
  "%ProgramFiles%\Streamlabs OBS"
  "%ProgramFiles%\Streamlabs Desktop"
  "%ProgramFiles(x86)%\Streamlabs OBS"
  "%ProgramFiles(x86)%\Streamlabs Desktop"
) do (
  if exist "%%~R\%SLSUB%\obs-plugins\64bit" (
    call :add_obs_target "%%~R\%SLSUB%" "Streamlabs Desktop"
  )
)
REM  Non-standard install location: ask the uninstall entry where it went.
for /f "delims=" %%K in ('reg query "HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall" /f "Streamlabs" /k 2^>nul ^| findstr /i "HKEY_"') do (
  for /f "tokens=2,*" %%a in ('reg query "%%K" /v InstallLocation 2^>nul ^| findstr /i "REG_SZ"') do (
    if exist "%%b\%SLSUB%\obs-plugins\64bit" call :add_obs_target "%%b\%SLSUB%" "Streamlabs Desktop"
  )
)
exit /b 0

:check_running
REM  %1 = image name, %2 = friendly name. A loaded DLL cannot be replaced.
tasklist /fi "imagename eq %~1" 2>nul | findstr /i "%~1" >nul
if errorlevel 1 exit /b 0
echo   [x] %~2 is running. Close it first - Windows will not let the
echo       plugin DLL be replaced while it is loaded.
exit /b 1

:usage
echo   Usage: install.bat [/obs] [/slobs] [/dir "path"] [/uninstall]
echo.
echo     ^(no option^)   install into OBS Studio and Streamlabs Desktop
echo     /obs           OBS Studio only
echo     /slobs         Streamlabs Desktop only
echo     /dir "path"    a specific OBS install directory
echo     /uninstall     remove the plugin from the apps it finds
echo     /force         install even while OBS is running ^(copy may fail^)
echo.
goto done

:cancelled
echo   Cancelled - nothing was changed.
echo.
goto done

:fail
echo.
set "EXITCODE=1"
goto end

:done
set "EXITCODE=0"
goto end

:end
REM  Pause when double-clicked, so the window does not vanish.
echo %cmdcmdline% | findstr /i /c:"/c" >nul && pause
endlocal & exit /b %EXITCODE%
