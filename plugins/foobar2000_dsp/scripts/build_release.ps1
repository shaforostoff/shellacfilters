<#
.SYNOPSIS
    Builds the optimised release binaries and packages each component as an
    installable .fb2k-component.

.DESCRIPTION
    Configures and builds every requested architecture, runs the test suites,
    then assembles one archive per component containing all of them:

        foo_dsp_decrackle.fb2k-component
          foo_dsp_decrackle.dll        <- 32 bit, used by foobar2000 1.5 - 2.x (x86)
          x64/foo_dsp_decrackle.dll    <- 64 bit, used by foobar2000 2.x (x64)

    foobar2000 ignores subfolders it does not understand, so one file installs
    everywhere. Debug symbols go into a separate archive that is NOT part of
    the component - keep it around so foobar2000 crash reports can be resolved.

.PARAMETER Component
    Which components to build. Default: all of them.

.PARAMETER Arch
    Which architectures to build. Default: x86 and x64.

.PARAMETER Toolset
    MSVC platform toolset. Leave unset for the newest installed one. Use v142
    (Visual Studio 2019, or the "MSVC v142 build tools" component of Visual
    Studio 2022) if the binaries have to run on Windows 7 - v143 from Visual
    Studio 2022 17.10 onwards no longer supports it. See -Win7, which picks a
    suitable toolset by itself.

.PARAMETER Generator
    CMake generator. Default: whatever CMake picks for the installed VS.

.PARAMETER Win7
    Build something that loads on Windows 7: pick the newest installed MSVC
    toolset that still supports it (v142, else v141), and run
    scripts\check_win7.ps1 over the finished components, failing the build if
    anything in them needs a newer Windows. Implies the SSE2 baseline and the
    static CRT.

.PARAMETER WindowsSdk
    Windows SDK version, e.g. 10.0.17763.0. Default: whatever CMake picks,
    which is the newest installed. The SDK version is not what decides which
    Windows versions the binary runs on - _WIN32_WINNT and the toolset are -
    so there is rarely a reason to set this.

.PARAMETER InstructionSet
    SSE2 (default, runs anywhere Windows 7 does), AVX or AVX2. Only pick a
    higher one if the machine that will run it definitely supports it - an
    unsupported instruction is an instant crash, not a graceful failure.

.PARAMETER SkipTests
    Do not run the verification harnesses. Not recommended.

.PARAMETER Clean
    Wipe the build directories first.

.EXAMPLE
    .\scripts\build_release.ps1

.EXAMPLE
    # Windows 7 compatible build, verified before it is packaged
    .\scripts\build_release.ps1 -Win7
#>

[CmdletBinding()]
param(
    [ValidateSet('x86', 'x64')]
    [string[]] $Arch = @('x86', 'x64'),
    [ValidateSet('foo_dsp_decrackle', 'foo_dsp_declick', 'foo_dsp_dehum', 'foo_dsp_paraeq')]
    [string[]] $Component = @('foo_dsp_decrackle', 'foo_dsp_declick', 'foo_dsp_dehum', 'foo_dsp_paraeq'),
    [string]   $Toolset = '',
    [string]   $Generator = '',
    [ValidateSet('SSE2', 'AVX', 'AVX2')]
    [string]   $InstructionSet = 'SSE2',
    [switch]   $Win7,
    [string]   $WindowsSdk = '',
    [switch]   $SkipTests,
    [switch]   $Clean
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root    = Split-Path -Parent $PSScriptRoot        # plugins/foobar2000_dsp
$plugins = Split-Path -Parent $root               # plugins
# One level above this project, because it is shared: scripts\build_winvst.ps1
# puts the VST2 DLLs in dist\winvst, and those are not foobar2000 components.
$distDir = Join-Path $plugins 'dist'
$stage   = Join-Path $root 'build\_package'
$symbols = Join-Path $root 'build\_symbols'

function Invoke-Checked([string] $what, [scriptblock] $action) {
    & $action
    if ($LASTEXITCODE -ne 0) { throw "$what failed with exit code $LASTEXITCODE" }
}

# --- Windows 7 toolset ------------------------------------------------------
# v143 dropped Windows 7 as a target in Visual Studio 2022 17.10. v142 and v141
# still support it, wherever they happen to be installed - and a toolset can
# only be driven by the generator of the Visual Studio it lives in, so the two
# have to be chosen together.
function Select-Win7Toolset {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }

    $json = & $vswhere -all -products * -format json
    if (-not $json) { return $null }

    # Newest Visual Studio first, so a v142 installed next to v143 in Visual
    # Studio 2022 is preferred over one in a separate 2019 installation.
    $installs = @(ConvertFrom-Json ($json -join "`n") | ForEach-Object {
        [pscustomobject] @{
            Path  = $_.installationPath
            Major = [int] (($_.installationVersion -split '\.')[0])
        }
    } | Sort-Object Major -Descending)

    foreach ($prefix in @('14.2', '14.1')) {      # v142, then v141
        foreach ($install in $installs) {
            $tools = Join-Path $install.Path 'VC\Tools\MSVC'
            if (-not (Test-Path $tools)) { continue }
            $hit = Get-ChildItem $tools -Directory -ErrorAction SilentlyContinue |
                       Where-Object { $_.Name.StartsWith($prefix) } |
                       Sort-Object Name -Descending | Select-Object -First 1
            if (-not $hit) { continue }

            $toolset = 'v141'
            if ($prefix -eq '14.2') { $toolset = 'v142' }
            $generator = ''
            switch ($install.Major) {
                17 { $generator = 'Visual Studio 17 2022' }
                16 { $generator = 'Visual Studio 16 2019' }
                15 { $generator = 'Visual Studio 15 2017' }
            }
            return [pscustomobject] @{
                Toolset   = $toolset
                Generator = $generator
                Compiler  = $hit.Name
                Install   = $install.Path
            }
        }
    }
    return $null
}

if ($Win7) {
    if ($InstructionSet -ne 'SSE2') {
        Write-Warning "-InstructionSet $InstructionSet with -Win7: $InstructionSet needs Windows 7 SP1 and a CPU that supports it."
    }
    if ($Toolset -or $Generator) {
        Write-Host "-Win7: keeping the toolset/generator given on the command line." -ForegroundColor Yellow
    }
    else {
        $pick = Select-Win7Toolset
        if ($pick) {
            $Toolset = $pick.Toolset
            if ($pick.Generator) { $Generator = $pick.Generator }
            Write-Host ("-Win7: using {0} (MSVC {1}) from {2}" -f
                $pick.Toolset, $pick.Compiler, $pick.Install) -ForegroundColor Cyan
        }
        else {
            Write-Warning ("-Win7: no v142 or v141 toolset found, falling back to the default one. " +
                "v143 no longer supports Windows 7 as a target; the check at the end of this build " +
                "still verifies the result, but Microsoft does not support it. Install the " +
                "`"MSVC v142 - VS 2019 C++ build tools`" component to get a supported build.")
        }
    }
}

# --- the cores and the VST wrapper are mirrored into the other formats; catch --
# --- drift before we build a component whose maths no longer matches the VSTs' -
Write-Host "`n=== Core sync ===" -ForegroundColor Cyan
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'sync_cores.ps1') -Check
if ($LASTEXITCODE -ne 0) { throw "mirrors have drifted; see above" }

# --- read each version out of version.h so archive names match the DLLs -----
$versions = @{}
foreach ($c in $Component) {
    $h = Join-Path $root "$c\version.h"
    $versions[$c] = (Select-String -Path $h -Pattern 'VERSION_STRING\s+"([^"]+)"').Matches[0].Groups[1].Value
    Write-Host ("{0} {1}" -f $c, $versions[$c]) -ForegroundColor Cyan
}

foreach ($dir in @($stage, $symbols)) {
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
}
New-Item -ItemType Directory -Force $stage, $symbols, $distDir | Out-Null

foreach ($a in $Arch) {
    $platform  = if ($a -eq 'x64') { 'x64' } else { 'Win32' }
    $buildDir  = Join-Path $root "build\$a"

    if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }

    Write-Host "`n=== Configuring $a ===" -ForegroundColor Cyan
    $cfg = @('-S', $root, '-B', $buildDir, '-A', $platform,
             "-DFOO_DSP_ARCH=$InstructionSet",
             '-DFOO_DSP_STATIC_CRT=ON',
             '-DFOO_DSP_LTO=ON',
             '-DCMAKE_BUILD_TYPE=Release')
    if ($Generator)  { $cfg += @('-G', $Generator) }
    if ($Toolset)    { $cfg += @('-T', $Toolset) }
    if ($WindowsSdk) { $cfg += "-DCMAKE_SYSTEM_VERSION=$WindowsSdk" }
    Invoke-Checked "cmake configure ($a)" { & cmake @cfg }

    Write-Host "`n=== Building $a ===" -ForegroundColor Cyan
    Invoke-Checked "cmake build ($a)" {
        & cmake --build $buildDir --config Release --parallel
    }

    if (-not $SkipTests) {
        Write-Host "`n=== Testing $a ===" -ForegroundColor Cyan
        Invoke-Checked "ctest ($a)" {
            & ctest --test-dir $buildDir -C Release --output-on-failure
        }
    }

    # 32 bit goes at the archive root, 64 bit in x64\ - that is the layout
    # foobar2000 2.x expects, and 1.5 simply ignores the subfolder.
    foreach ($c in $Component) {
        $subdir = if ($a -eq 'x64') { Join-Path $stage "$c\x64" } else { Join-Path $stage $c }
        New-Item -ItemType Directory -Force $subdir | Out-Null

        $built = Join-Path $buildDir "$c\Release\$c.dll"
        if (-not (Test-Path $built)) { throw "Expected output missing: $built" }
        Copy-Item $built $subdir -Force

        $pdb = [System.IO.Path]::ChangeExtension($built, '.pdb')
        if (Test-Path $pdb) {
            $symDir = Join-Path $symbols "$c\$a"
            New-Item -ItemType Directory -Force $symDir | Out-Null
            Copy-Item $pdb $symDir -Force
        }

        $info = Get-Item $built
        Write-Host ("  {0,-18} {1,-4} {2,9:N0} bytes" -f $c, $a, $info.Length) -ForegroundColor Green
    }
}

# --- Windows 7 verification -------------------------------------------------
# Before packaging, so a component that cannot load there is never produced.
if ($Win7) {
    Write-Host "`n=== Windows 7 check ===" -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'check_win7.ps1') -Path $stage
    if ($LASTEXITCODE -ne 0) {
        throw "The built components need something newer than Windows 7 - see above."
    }
}

# --- package ---------------------------------------------------------------
# Compress-Archive would work, but cmake -E tar produces the same zip on every
# PowerShell version and is already a hard dependency here.
Write-Host "`n=== Package ===" -ForegroundColor Cyan
foreach ($c in $Component) {
    $v = $versions[$c]
    $componentPath = Join-Path $distDir "$c-$v.fb2k-component"
    $symbolsPath   = Join-Path $distDir "$c-$v-symbols.zip"
    foreach ($p in @($componentPath, $symbolsPath)) {
        if (Test-Path $p) { Remove-Item -Force $p }
    }
    $src = Join-Path $stage $c
    Invoke-Checked "packaging $c" {
        & cmake -E chdir $src cmake -E tar cf $componentPath --format=zip .
    }
    $symSrc = Join-Path $symbols $c
    if (Test-Path $symSrc) {
        Invoke-Checked "packaging $c symbols" {
            & cmake -E chdir $symSrc cmake -E tar cf $symbolsPath --format=zip .
        }
    }
    Write-Host ("  {0}  ({1:N0} bytes)" -f $componentPath, (Get-Item $componentPath).Length) -ForegroundColor Green
    & cmake -E tar tf $componentPath | ForEach-Object { Write-Host "      $_" }
    if (Test-Path $symbolsPath) {
        Write-Host ("  {0}  ({1:N0} bytes)" -f $symbolsPath, (Get-Item $symbolsPath).Length) -ForegroundColor DarkGray
    }
}

Write-Host @"

To install: drag the .fb2k-component file onto foobar2000, or use
File > Preferences > Components > Install...
"@ -ForegroundColor Yellow
