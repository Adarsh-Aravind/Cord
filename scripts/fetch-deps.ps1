<#
.SYNOPSIS
  Downloads and prepares everything Cord needs to build and run.

.DESCRIPTION
  Four pieces, none of which belong in git:

    data/bin/            adb.exe + the scrcpy-server jar        (RUNTIME)
    deps/obs-headers/    libobs headers from the obs-studio tag (BUILD)
    deps/obs-deps/       FFmpeg (avcodec, avutil) prebuilt      (BUILD)
    deps/lib/obs.lib     import library generated from the
                         installed obs.dll                      (BUILD)

  Note what is NOT here: OBS itself is never built. libobs's only generated
  header, obsconfig.h, is three path macros and two version macros that no
  plugin code path reads, so it is synthesised below. The import library is
  produced from the obs.dll already on this machine, which means the plugin
  links against exactly the OBS it will run in - no ABI guesswork.

  Versions are pinned. The scrcpy device-server protocol changes between
  releases and src/device/scrcpy-proto.h is transcribed from one specific
  version; bumping one without the other produces a stream that reads as
  garbage. See docs/building.md.

.PARAMETER ObsDir
  OBS Studio install directory. Autodetected from the registry if omitted.

.PARAMETER RuntimeOnly
  Fetch only adb + scrcpy-server.
#>
[CmdletBinding()]
param(
    [string]$ObsDir,
    [switch]$RuntimeOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# --- pinned versions -------------------------------------------------------

$ScrcpyVersion  = '5.0.1'       # MUST match CORD_SCRCPY_VERSION in scrcpy-proto.h
$ObsVersion     = '32.2.2'      # headers; should match the installed OBS
# obsproject/obs-deps. This MUST carry the same FFmpeg major versions as the
# DLLs the installed OBS ships in bin4bit (avcodec-NN.dll, avutil-NN.dll),
# because that is what the plugin loads at runtime. 2026-10-23 = avcodec 62 /
# avutil 60, matching OBS 32.x. The version pinned by obs-plugintemplate is
# older (avcodec 61) and produces a plugin OBS cannot load.
$ObsDepsVersion = '2026-10-23'

$Root       = Split-Path -Parent $PSScriptRoot
$DepsDir    = Join-Path $Root 'deps'
$BinDir     = Join-Path $Root 'data/bin'
$CacheDir   = Join-Path $DepsDir '_cache'
$HeadersDir = Join-Path $DepsDir 'obs-headers'
$LibDir     = Join-Path $DepsDir 'lib'

function New-Dir([string]$Path) {
    if (-not (Test-Path $Path)) { New-Item -ItemType Directory -Force -Path $Path | Out-Null }
}

function Get-File([string]$Url, [string]$OutFile) {
    if (Test-Path $OutFile) { Write-Host "  cached:      $(Split-Path -Leaf $OutFile)"; return }
    Write-Host "  downloading: $Url"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing
}

New-Dir $DepsDir; New-Dir $BinDir; New-Dir $CacheDir

# ==========================================================================
# 1. runtime: scrcpy device server + adb
# ==========================================================================
Write-Host "`n[1/4] scrcpy $ScrcpyVersion (device server + adb)" -ForegroundColor Cyan

$scrcpyZipName = "scrcpy-win64-v$ScrcpyVersion.zip"
$scrcpyZip = Join-Path $CacheDir $scrcpyZipName
Get-File "https://github.com/Genymobile/scrcpy/releases/download/v$ScrcpyVersion/$scrcpyZipName" $scrcpyZip

$scrcpyTmp = Join-Path $CacheDir 'scrcpy-extract'
if (Test-Path $scrcpyTmp) { Remove-Item -Recurse -Force $scrcpyTmp }
Expand-Archive -Path $scrcpyZip -DestinationPath $scrcpyTmp -Force

$scrcpyRoot = Get-ChildItem -Directory $scrcpyTmp | Select-Object -First 1
if (-not $scrcpyRoot) { $scrcpyRoot = Get-Item $scrcpyTmp }

foreach ($name in @('scrcpy-server', 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll')) {
    $src = Join-Path $scrcpyRoot.FullName $name
    if (-not (Test-Path $src)) { throw "'$name' missing from $scrcpyZipName - did the release layout change?" }
    Copy-Item $src (Join-Path $BinDir $name) -Force
    Write-Host "  installed:   $name"
}

@{  scrcpy_version = $ScrcpyVersion
    fetched_utc    = (Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json | Set-Content (Join-Path $BinDir 'VERSION.json') -Encoding utf8

if ($RuntimeOnly) {
    Write-Host "`nRuntime dependencies ready." -ForegroundColor Green
    exit 0
}

# ==========================================================================
# 2. libobs headers
# ==========================================================================
Write-Host "`n[2/4] libobs headers (obs-studio $ObsVersion)" -ForegroundColor Cyan

$obsZip = Join-Path $CacheDir "obs-studio-$ObsVersion.zip"
Get-File "https://github.com/obsproject/obs-studio/archive/refs/tags/$ObsVersion.zip" $obsZip

$obsTmp = Join-Path $CacheDir 'obs-extract'
if (Test-Path $obsTmp) { Remove-Item -Recurse -Force $obsTmp }
Expand-Archive -Path $obsZip -DestinationPath $obsTmp -Force

$obsSrcRoot = Get-ChildItem -Directory $obsTmp | Select-Object -First 1
$libobsSrc = Join-Path $obsSrcRoot.FullName 'libobs'
if (-not (Test-Path $libobsSrc)) { throw "libobs/ not found in the obs-studio $ObsVersion archive" }

if (Test-Path $HeadersDir) { Remove-Item -Recurse -Force $HeadersDir }
New-Dir $HeadersDir
Copy-Item $libobsSrc (Join-Path $HeadersDir 'libobs') -Recurse -Force

# Keep only headers: the .c files would confuse nothing, but shipping 40MB of
# sources we never compile makes the tree harder to reason about.
Get-ChildItem (Join-Path $HeadersDir 'libobs') -Recurse -File |
    Where-Object { $_.Extension -notin @('.h', '.hpp', '.in') } |
    Remove-Item -Force
Write-Host "  installed:   deps/obs-headers/libobs"

# libobs/util/threading.h does #include <pthread.h>, and on Windows that is
# OBS's bundled w32-pthreads shim, which lives OUTSIDE libobs/ in the source
# tree. Without these headers nothing that touches threading.h will compile.
$pthreadSrc = Join-Path $obsSrcRoot.FullName 'deps/w32-pthreads'
if (-not (Test-Path $pthreadSrc)) {
    throw "deps/w32-pthreads not found in the obs-studio $ObsVersion archive"
}
$pthreadDst = Join-Path $HeadersDir 'w32-pthreads'
New-Dir $pthreadDst
Copy-Item (Join-Path $pthreadSrc '*.h') $pthreadDst -Force
Write-Host "  installed:   deps/obs-headers/w32-pthreads (pthread.h)"

# obsconfig.h: generated by OBS's CMake. These macros are only read by libobs's
# own translation units (already compiled into obs.dll), so the values are
# irrelevant to us - they just have to exist for the headers to compile.
$obsconfig = @"
/* Generated by scripts/fetch-deps.ps1 - do not edit.
 *
 * Stands in for the obsconfig.h that OBS's own CMake generates. Only libobs's
 * internal sources read these; a plugin just needs them defined.
 */
#pragma once

#define OBS_DATA_PATH "../../data"
#define OBS_PLUGIN_PATH "../../obs-plugins/64bit"
#define OBS_PLUGIN_DESTINATION "obs-plugins/64bit"

#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
"@
Set-Content -Path (Join-Path $HeadersDir 'libobs/obsconfig.h') -Value $obsconfig -Encoding utf8
Write-Host "  synthesised: obsconfig.h"

# ==========================================================================
# 3. FFmpeg (and friends) from obs-deps
# ==========================================================================
Write-Host "`n[3/4] obs-deps $ObsDepsVersion (FFmpeg)" -ForegroundColor Cyan

$depsZipName = "windows-deps-$ObsDepsVersion-x64.zip"
$depsZip = Join-Path $CacheDir $depsZipName
Get-File "https://github.com/obsproject/obs-deps/releases/download/$ObsDepsVersion/$depsZipName" $depsZip

$obsDepsDir = Join-Path $DepsDir 'obs-deps'
if (Test-Path $obsDepsDir) { Remove-Item -Recurse -Force $obsDepsDir }
Expand-Archive -Path $depsZip -DestinationPath $obsDepsDir -Force

foreach ($probe in @('include/libavcodec/avcodec.h', 'lib/avcodec.lib')) {
    if (-not (Test-Path (Join-Path $obsDepsDir $probe))) {
        Write-Warning "  expected '$probe' in obs-deps; the archive layout may have changed"
    }
}
Write-Host "  installed:   deps/obs-deps"

# ==========================================================================
# 4. obs.lib, generated from the installed obs.dll
# ==========================================================================
Write-Host "`n[4/4] obs.lib import library" -ForegroundColor Cyan

if (-not $ObsDir) {
    $keys = @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
              'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*')
    # Most uninstall keys lack DisplayName/InstallLocation entirely, and
    # StrictMode makes touching an absent property a hard error - so check that
    # each property exists before reading it.
    $entry = Get-ItemProperty $keys -ErrorAction SilentlyContinue |
             Where-Object {
                 $props = $_.PSObject.Properties.Name
                 ($props -contains 'DisplayName') -and
                 ($props -contains 'InstallLocation') -and
                 ($_.DisplayName -like '*OBS Studio*') -and
                 $_.InstallLocation
             } |
             Select-Object -First 1
    if ($entry) { $ObsDir = $entry.InstallLocation }
}
if (-not $ObsDir) {
    foreach ($guess in @("$env:ProgramFiles\obs-studio",
                         "${env:ProgramFiles(x86)}\obs-studio")) {
        if (Test-Path $guess) { $ObsDir = $guess; break }
    }
}
if (-not $ObsDir -or -not (Test-Path $ObsDir)) {
    throw "Could not find OBS Studio. Pass -ObsDir 'C:\path\to\obs-studio'."
}

$obsDll = Join-Path $ObsDir 'bin\64bit\obs.dll'
if (-not (Test-Path $obsDll)) { throw "obs.dll not found at $obsDll" }
Write-Host "  OBS install: $ObsDir"

# The plugin links FFmpeg by soname (avcodec-NN.dll). If obs-deps and the
# installed OBS disagree on the major version, the DLL builds fine and then
# fails to load with no useful message - so check it here instead.
$obsBin = Join-Path $ObsDir 'bin\64bit'
foreach ($pair in @(
        @{ Lib = 'libavcodec'; Dll = 'avcodec' },
        @{ Lib = 'libavutil';  Dll = 'avutil'  })) {

    $verHeader = Join-Path $obsDepsDir "include/$($pair.Lib)/version_major.h"
    if (-not (Test-Path $verHeader)) {
        $verHeader = Join-Path $obsDepsDir "include/$($pair.Lib)/version.h"
    }
    $depsMajor = (Select-String -Path $verHeader -Pattern 'VERSION_MAJOR\s+(\d+)' |
                  Select-Object -First 1).Matches[0].Groups[1].Value

    $shipped = Get-ChildItem $obsBin -Filter "$($pair.Dll)-*.dll" -ErrorAction SilentlyContinue |
               Select-Object -First 1
    if (-not $shipped) { throw "OBS does not ship any $($pair.Dll)-*.dll in $obsBin" }
    $obsMajor = [regex]::Match($shipped.Name, '-(\d+)\.dll$').Groups[1].Value

    if ($depsMajor -ne $obsMajor) {
        throw @"
FFmpeg version mismatch for $($pair.Dll):
  obs-deps $ObsDepsVersion provides major $depsMajor
  this OBS ships            $($shipped.Name) (major $obsMajor)

The plugin would build and then fail to load. Pick the obs-deps release whose
FFmpeg major matches your OBS and update `$ObsDepsVersion at the top of this
script. Releases: https://github.com/obsproject/obs-deps/releases
"@
    }
    Write-Host "  ffmpeg:      $($pair.Dll) major $obsMajor matches OBS"
}

# dumpbin and lib live in the MSVC toolchain; locate it through vswhere.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found - is Visual Studio installed?' }

# '*' must be quoted: unquoted, PowerShell tries to glob it against the current
# directory and vswhere receives something it cannot match, returning nothing
# even when the workload IS installed.
$vsPath = & $vswhere -latest -prerelease -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath

$vcvars = $null
if ($vsPath) {
    $candidate = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path $candidate) { $vcvars = $candidate }
}

if (-not $vcvars) {
    # Fallback: vswhere can also come up empty on preview/odd channels, so look
    # for vcvars64.bat directly before declaring the toolchain missing.
    $vcvars = Get-ChildItem -Path @(
            "$env:ProgramFiles\Microsoft Visual Studio",
            "${env:ProgramFiles(x86)}\Microsoft Visual Studio"
        ) -Filter 'vcvars64.bat' -Recurse -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}

if (-not $vcvars) {
    throw @"
The Visual Studio C++ workload is not installed, so dumpbin and lib are
unavailable. Install it (Visual Studio Installer -> Modify -> Desktop
development with C++) and re-run this script. See docs/building.md.
"@
}
Write-Host "  toolchain:   $vcvars"

New-Dir $LibDir

function New-ImportLib([string]$DllPath, [string]$BaseName, [int]$MinExports) {
    $defFile    = Join-Path $LibDir "$BaseName.def"
    $libFile    = Join-Path $LibDir "$BaseName.lib"
    $exportsTxt = Join-Path $CacheDir "$BaseName-exports.txt"

    # One cmd invocation: set up the MSVC environment, then dump the exports.
    & cmd /c "`"$vcvars`" >nul 2>&1 && dumpbin /exports `"$DllPath`"" > $exportsTxt
    if (-not (Test-Path $exportsTxt)) { throw "dumpbin produced no output for $BaseName" }

    # dumpbin's export table rows are: ordinal hint RVA name
    $names = Get-Content $exportsTxt |
        Where-Object { $_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)' } |
        ForEach-Object { $Matches[1] }

    if ($names.Count -lt $MinExports) {
        throw "Only $($names.Count) exports parsed from $BaseName.dll (expected >= $MinExports) - dumpbin output format may have changed. See $exportsTxt"
    }

    Set-Content -Path $defFile -Value (@("LIBRARY $BaseName", 'EXPORTS') + ($names | Sort-Object -Unique)) -Encoding ascii

    & cmd /c "`"$vcvars`" >nul 2>&1 && lib /nologo /def:`"$defFile`" /out:`"$libFile`" /machine:x64" | Out-Null
    if (-not (Test-Path $libFile)) { throw "lib failed to produce $BaseName.lib" }

    Write-Host "  generated:   deps/lib/$BaseName.lib  ($($names.Count) symbols)"
}

New-ImportLib $obsDll 'obs' 500

# libobs's pthread functions live in their own DLL on Windows (the w32-pthreads
# shim), NOT in obs.dll - so anything using pthread_create or pthread_mutex_*
# needs this import library too.
$pthreadDll = Join-Path $ObsDir 'bin\64bit\w32-pthreads.dll'
if (-not (Test-Path $pthreadDll)) { throw "w32-pthreads.dll not found at $pthreadDll" }
New-ImportLib $pthreadDll 'w32-pthreads' 50

# Record what the build is pinned against.
@{  obs_version      = $ObsVersion
    obs_install_dir  = $ObsDir
    obs_deps_version = $ObsDepsVersion
    scrcpy_version   = $ScrcpyVersion
    prepared_utc     = (Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json | Set-Content (Join-Path $DepsDir 'VERSION.json') -Encoding utf8

Write-Host "`nDone." -ForegroundColor Green
Write-Host @"

Next:
  cmake --preset windows-x64
  cmake --build --preset windows-x64
  cmake --install build --prefix "$ObsDir"
"@
