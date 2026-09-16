<#
.SYNOPSIS
    Builds the Windows VST2 plug-ins in plugins/WinVST into release DLLs.

.DESCRIPTION
    Produces, for each plug-in and architecture:

        plugins\dist\winvst\Declick32.dll   <- 32 bit hosts
        plugins\dist\winvst\Declick64.dll   <- 64 bit hosts
        plugins\dist\winvst\Dehum32.dll
        plugins\dist\winvst\Dehum64.dll
        plugins\dist\winvst\ParaEQ32.dll
        plugins\dist\winvst\ParaEQ64.dll

    A VST2 host identifies a plug-in by its uniqueID rather than its filename,
    so the 32 and 64 bit builds can sit in the same VST folder.

    Two things about this script are worth knowing before reading it.

    It does not use each plug-in's VSTProject.vcxproj. Those ask for platform
    toolset v140 and Windows SDK 8.1, and they reference Steinberg's vst2.x
    sources at a path that is not in this repository. Rather than rewrite
    Airwindows' project files - which are what the documented "drag the folder
    into VSTProject and press build" route needs, and which are left exactly as
    shipped - this drives cl.exe directly.

    It links plugins/WinVST/vst2_shim, a clean-room implementation of the VST2
    ABI, in place of the SDK. Read plugins/WinVST/vst2_shim/README.md for what
    that is and, more to the point, what it does not prove.

    Unless -SkipTests is given, each finished DLL is then handed to
    tests/vst_host_verify.cpp, which loads it the way a host does - through
    LoadLibrary and the C ABI and nothing else - and requires its audio to match
    the same plug-in linked statically, to the bit. That test is shared with
    scripts/build_linuxvst.sh, which does the same for the LinuxVST ports -
    for Declick and Dehum, which are the two plug-ins that have a LinuxVST port.
    ParaEQ is built here only.

.PARAMETER Plugin
    Which plug-ins to build. Default: both.

.PARAMETER Arch
    Which architectures. Default: x86 and x64.

.PARAMETER SkipTests
    Build the DLLs but do not load and verify them. Not recommended: the host
    test is the only thing here that exercises the ABI.

.PARAMETER Clean
    Wipe the intermediate directory first.

.EXAMPLE
    .\scripts\build_winvst.ps1

.EXAMPLE
    # just the 64 bit dehummer, quickly
    .\scripts\build_winvst.ps1 -Plugin Dehum -Arch x64
#>

[CmdletBinding()]
param(
    [ValidateSet('Declick', 'Dehum', 'ParaEQ')]
    [string[]] $Plugin = @('Declick', 'Dehum', 'ParaEQ'),
    [ValidateSet('x86', 'x64')]
    [string[]] $Arch = @('x86', 'x64'),
    [switch]   $SkipTests,
    [switch]   $Clean
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root     = Split-Path -Parent $PSScriptRoot        # plugins/foobar2000_dsp
$plugins  = Split-Path -Parent $root               # plugins
$winvst   = Join-Path $plugins 'WinVST'
$shimDir  = Join-Path $winvst 'vst2_shim'
$testsDir = Join-Path $root 'tests'
$buildDir = Join-Path $root 'build\winvst'
# alongside build_release.ps1's .fb2k-components, one level above this project,
# because a VST2 DLL is not a foobar2000 component
$outDir   = Join-Path $plugins 'dist\winvst'

# ---------------------------------------------------------------------------
# helpers

function Invoke-Native {
    param(
        [Parameter(Mandatory)] [string]   $Exe,
        [Parameter(Mandatory)] [string[]] $Arguments,
        [Parameter(Mandatory)] [string]   $What
    )
    # cl and link write diagnostics to stdout; keep PowerShell from turning a
    # stderr line into a terminating error and losing the real exit code
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $Exe @Arguments
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $saved
    }

    # warnings are not fatal but must not be invisible either
    $warnings = @($output | Where-Object { $_ -match ':\s+warning\s+[A-Z]+\d+' })
    foreach ($w in $warnings) { Write-Host ("      " + $w.Trim()) -ForegroundColor DarkYellow }

    if ($code -ne 0) {
        foreach ($line in $output) { Write-Host $line }
        throw "$What failed (exit $code)"
    }
    return $warnings.Count
}

function Get-VcVarsAll {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found. Visual Studio 2017 or newer is required."
    }
    $installs = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $installs) { throw "no Visual Studio with the C++ toolset was found." }
    $path = Join-Path ($installs | Select-Object -First 1) 'VC\Auxiliary\Build\vcvarsall.bat'
    if (-not (Test-Path $path)) { throw "vcvarsall.bat not found at $path" }
    return $path
}

$envSnapshot = @{}
Get-ChildItem Env: | ForEach-Object { $envSnapshot[$_.Name] = $_.Value }

function Restore-Environment {
    foreach ($existing in @(Get-ChildItem Env: | Select-Object -ExpandProperty Name)) {
        if (-not $envSnapshot.ContainsKey($existing)) { Remove-Item ("Env:" + $existing) }
    }
    foreach ($name in $envSnapshot.Keys) {
        Set-Item -Path ("Env:" + $name) -Value $envSnapshot[$name]
    }
}

function Enter-MsvcEnvironment {
    param([Parameter(Mandatory)] [string] $VcVarsAll,
          [Parameter(Mandatory)] [string] $TargetArch)
    Restore-Environment
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        # vcvarsall prints a banner, and its internals complain about a vswhere
        # that is not on PATH; only the variable dump after it is wanted. The
        # exit code and the cl.exe check below are what catch a real failure.
        $dump = & cmd /c "call `"$VcVarsAll`" $TargetArch >nul 2>nul && set"
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $saved
    }
    if ($code -ne 0) { throw "vcvarsall.bat $TargetArch failed (exit $code)" }
    foreach ($line in $dump) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2]
        }
    }
    if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
        throw "cl.exe is still not on PATH after vcvarsall.bat $TargetArch"
    }
}

function Get-MachineType {
    param([Parameter(Mandatory)] [string] $Image)
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $headers = & dumpbin /nologo /headers $Image } finally { $ErrorActionPreference = $saved }
    $line = $headers | Where-Object { $_ -match 'machine \((x64|x86)\)' } | Select-Object -First 1
    if ($line -match 'machine \((x64|x86)\)') { return $matches[1] }
    return 'unknown'
}

# ---------------------------------------------------------------------------
# the same compiler settings for every target
#
# Deliberately not a copy of the .vcxproj's Release settings: that project turns
# off intrinsic functions and function level linking, which are Chris Johnson's
# choices for his own build and cost measurable speed in a DSP core. What is kept
# is everything that changes results or ABI - /fp:precise (the default, named
# here because /fp:fast would quietly change the audio and break the bit
# comparisons), the static CRT so a host needs no redistributable, and the SSE2
# baseline the rest of this repository targets.

$commonDefines = @(
    '/D', 'WINDOWS', '/D', '_WINDOWS', '/D', 'WIN32', '/D', '_USRDLL',
    '/D', '_USE_MATH_DEFINES', '/D', '_CRT_SECURE_NO_DEPRECATE',
    '/D', 'VST_FORCE_DEPRECATED'
)
$commonFlags = @('/nologo', '/c', '/W3', '/EHsc', '/O2', '/Ot', '/Oi', '/MT',
                 '/GS', '/Gy', '/fp:precise', '/std:c++14')

function Get-ArchFlags {
    param([Parameter(Mandatory)] [string] $TargetArch)
    if ($TargetArch -eq 'x86') { return @('/arch:SSE2') }
    return @()
}

# ---------------------------------------------------------------------------
# What a plug-in needs beyond the four files every one of them has.
#
# Only ParaEQ has anything: it ships an editor, which is two more sources -
# ParaEQEditor.cpp, the join between AEffEditor and the editor's Host
# interface, and paraeq_editor.cpp, the owner-drawn curve itself, mirrored in
# from foo_dsp_paraeq by sync_cores. Declick and Dehum have no editor and get
# an empty list, which is also why they still link against nothing but the CRT.

function Get-ExtraSources {
    param([Parameter(Mandatory)] [string] $Plugin,
          [Parameter(Mandatory)] [string] $PluginDir)
    if ($Plugin -eq 'ParaEQ') {
        return @((Join-Path $PluginDir 'ParaEQEditor.cpp'),
                 (Join-Path $PluginDir 'paraeq_editor.cpp'))
    }
    return @()
}

# The Win32 libraries an editor pulls in. A DLL that draws nothing links
# against the CRT alone, so this is empty for Declick and Dehum and the two of
# them keep the smallest import table they had.
function Get-ExtraLibs {
    param([Parameter(Mandatory)] [string] $Plugin)
    if ($Plugin -eq 'ParaEQ') { return @('user32.lib', 'gdi32.lib') }
    return @()
}

# ---------------------------------------------------------------------------

Write-Host "`nmirrored cores" -ForegroundColor Cyan
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'sync_cores.ps1') -Check
if ($LASTEXITCODE -ne 0) {
    throw "a mirrored core does not match the canonical copy - fix that before building anything"
}

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "`nremoving $buildDir" -ForegroundColor DarkGray
    Remove-Item -Recurse -Force $buildDir
}
foreach ($dir in @($buildDir, $outDir)) {
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
}

$vcVarsAll = Get-VcVarsAll
Write-Host ("`nvcvarsall: {0}" -f $vcVarsAll) -ForegroundColor DarkGray

$results = @()
$totalWarnings = 0

try {
    foreach ($a in $Arch) {
        Write-Host "`n=== $a ===" -ForegroundColor Cyan
        Enter-MsvcEnvironment -VcVarsAll $vcVarsAll -TargetArch $a

        Write-Host ("  {0}" -f (Get-Command cl).Source) -ForegroundColor DarkGray

        $suffix    = if ($a -eq 'x86') { '32' } else { '64' }
        $archFlags = Get-ArchFlags -TargetArch $a

        foreach ($p in $Plugin) {
            $pluginDir = Join-Path $winvst $p
            if (-not (Test-Path $pluginDir)) { throw "no such plug-in directory: $pluginDir" }

            $coreName = ($p.ToLower() + '_core.cpp')
            $defFile  = Join-Path $pluginDir 'vstplug.def'
            $objDir   = Join-Path $buildDir "$a\$p"
            if (-not (Test-Path $objDir)) { New-Item -ItemType Directory -Force $objDir | Out-Null }

            $sources = @(
                (Join-Path $shimDir 'audioeffectx.cpp'),
                (Join-Path $shimDir 'vstplugmain.cpp'),
                (Join-Path $pluginDir "$p.cpp"),
                (Join-Path $pluginDir "${p}Proc.cpp"),
                (Join-Path $pluginDir $coreName)
            ) + (Get-ExtraSources -Plugin $p -PluginDir $pluginDir)
            foreach ($s in $sources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

            Write-Host ("`n  {0}{1}.dll" -f $p, $suffix) -ForegroundColor White

            $clArgs = $commonFlags + $archFlags + $commonDefines +
                      @('/I', $shimDir, '/I', $pluginDir, ('/Fo' + $objDir + '\')) + $sources
            $totalWarnings += Invoke-Native -Exe 'cl' -Arguments $clArgs -What "compiling $p ($a)"

            $dll  = Join-Path $outDir ("{0}{1}.dll" -f $p, $suffix)
            $objs = @(Get-ChildItem (Join-Path $objDir '*.obj') | Select-Object -ExpandProperty FullName)
            $linkArgs = @('/nologo', '/DLL', '/INCREMENTAL:NO', '/OPT:REF', '/OPT:ICF',
                          ('/DEF:' + $defFile), ('/OUT:' + $dll),
                          ('/IMPLIB:' + (Join-Path $objDir 'plug.lib'))) +
                        (Get-ExtraLibs -Plugin $p) + $objs
            $totalWarnings += Invoke-Native -Exe 'link' -Arguments $linkArgs -What "linking $p ($a)"

            $machine = Get-MachineType -Image $dll
            $wantMachine = $a
            if ($machine -ne $wantMachine) {
                throw ("{0} is machine {1}, expected {2}" -f $dll, $machine, $wantMachine)
            }
            $kb = [math]::Round((Get-Item $dll).Length / 1024.0, 1)
            Write-Host ("      {0} kB, machine {1}" -f $kb, $machine) -ForegroundColor DarkGray

            # --- the host test -------------------------------------------------
            $testResult = 'skipped'
            if (-not $SkipTests) {
                $testObjDir = Join-Path $buildDir "$a\${p}_hosttest"
                if (-not (Test-Path $testObjDir)) {
                    New-Item -ItemType Directory -Force $testObjDir | Out-Null
                }
                $testExe = Join-Path $testObjDir ("vst_host_verify_{0}{1}.exe" -f $p, $suffix)
                $testSources = @(
                    (Join-Path $testsDir 'vst_host_verify.cpp'),
                    (Join-Path $shimDir 'audioeffectx.cpp'),
                    (Join-Path $pluginDir "$p.cpp"),
                    (Join-Path $pluginDir "${p}Proc.cpp"),
                    (Join-Path $pluginDir $coreName)
                ) + (Get-ExtraSources -Plugin $p -PluginDir $pluginDir)
                # the test links the plug-in statically as well as loading the DLL,
                # so it must not also pull in vstplugmain.cpp - two definitions of
                # the entry point, and nothing here needs an exported one
                $testArgs = $commonFlags + $archFlags + $commonDefines +
                            @('/D', ('VST_PLUGIN_' + $p.ToUpper()),
                              '/I', $shimDir, '/I', $pluginDir,
                              ('/Fo' + $testObjDir + '\')) + $testSources
                $totalWarnings += Invoke-Native -Exe 'cl' -Arguments $testArgs `
                    -What "compiling vst_host_verify for $p ($a)"

                $testObjs = @(Get-ChildItem (Join-Path $testObjDir '*.obj') |
                              Select-Object -ExpandProperty FullName)
                $testLink = @('/nologo', '/INCREMENTAL:NO', ('/OUT:' + $testExe),
                              'psapi.lib') + (Get-ExtraLibs -Plugin $p) + $testObjs
                $totalWarnings += Invoke-Native -Exe 'link' -Arguments $testLink `
                    -What "linking vst_host_verify for $p ($a)"

                Write-Host ""
                $saved = $ErrorActionPreference
                $ErrorActionPreference = 'Continue'
                try {
                    & $testExe $dll
                    $testCode = $LASTEXITCODE
                } finally { $ErrorActionPreference = $saved }
                if ($testCode -ne 0) { throw ("vst_host_verify failed for {0} ({1})" -f $p, $a) }
                $testResult = 'passed'
            }

            $results += [pscustomobject]@{
                Plugin = $p
                Arch   = $a
                Dll    = $dll
                KB     = $kb
                Host   = $testResult
            }
        }
    }
} finally {
    Restore-Environment
}

# ---------------------------------------------------------------------------

Write-Host "`n=== built ===" -ForegroundColor Cyan
foreach ($r in $results) {
    Write-Host ("  {0,-14} {1,-4} {2,8} kB   host test {3}" -f `
        (Split-Path -Leaf $r.Dll), $r.Arch, $r.KB, $r.Host)
}
Write-Host ("`n{0} DLL(s) in {1}" -f @($results).Count, $outDir) -ForegroundColor Green
if ($totalWarnings -gt 0) {
    Write-Host ("{0} compiler warning(s) above" -f $totalWarnings) -ForegroundColor DarkYellow
}
Write-Host "copy them into your host's VST2 folder to install." -ForegroundColor DarkGray
exit 0
