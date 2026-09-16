<#
.SYNOPSIS
    Keeps the files more than one plug-in format compiles byte-identical
    across those formats.

.DESCRIPTION
    A core - declick_core.{h,cpp} and friends - deliberately knows nothing about
    foobar2000, VST or Win32, so every format can compile the same file. What it
    cannot be is a shared file on disk: a WinVST folder has to stand on its own,
    because the Airwindows build is "drag the plug-in's files into VSTProject and
    press build", and the folder that gets committed is the folder that was
    dragged. LinuxVST keeps a folder per plug-in for the same reason. So each
    format gets a copy, and this script is what stops the copies from drifting.

    The same goes for the VST2 wrapper - Declick.{h,cpp} and DeclickProc.cpp -
    which the Windows and Linux ports both compile unchanged.

    Run with no arguments to push each canonical copy out to its mirrors; run
    with -Check to compare only, which is what build_release.ps1 does before it
    builds anything.

    Adding a format means adding its directory to the To list of every file it
    builds, or a whole new entry if it wants files no other format does.

.PARAMETER Check
    Report differences and exit 1 instead of copying. Nothing is written.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\sync_cores.ps1

.EXAMPLE
    # what CI wants: fail if a mirror has been edited behind the canonical copy
    powershell -ExecutionPolicy Bypass -File scripts\sync_cores.ps1 -Check
#>

[CmdletBinding()]
param(
    [switch] $Check
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root    = Split-Path -Parent $PSScriptRoot          # plugins/foobar2000_dsp
$plugins = Split-Path -Parent $root                  # plugins

# canonical source directory -> files -> directories that must hold a copy
#
# The cores are canonical in the foobar2000 tree, which is where they are
# developed and where the test harness lives. The VST2 wrapper has no copy there
# - foo_dsp_* is a foobar2000 component, not a VST - so for those three files
# WinVST is canonical and the other VST2 ports mirror it.
#
# MacAU takes the cores but not the wrapper: an Audio Unit is a different
# interface, so its .cpp is its own source rather than a copy of anybody's.
# vdjplugin is in the same position for the same reason, and one folder there
# serves the two plug-ins it builds out of each core - the live one and the
# buffer one.
#
# ParaEQ is WinVST only so far, so its core has one mirror rather than five and
# there is no wrapper entry for it below: those exist to keep the other VST2
# ports in step with WinVST, and it has none yet.
$mirrors = @(
    @{
        From  = Join-Path $root 'foo_dsp_declick'
        Files = @('declick_core.h', 'declick_core.cpp')
        To    = @((Join-Path $plugins 'WinVST\Declick'),
                  (Join-Path $plugins 'LinuxVST\src\Declick'),
                  (Join-Path $plugins 'MacVST\Declick\source'),
                  (Join-Path $plugins 'MacAU\Declick'),
                  (Join-Path $plugins 'vdjplugin\vdj_declick'))
    },
    @{
        From  = Join-Path $root 'foo_dsp_dehum'
        Files = @('dehum_core.h', 'dehum_core.cpp')
        To    = @((Join-Path $plugins 'WinVST\Dehum'),
                  (Join-Path $plugins 'LinuxVST\src\Dehum'),
                  (Join-Path $plugins 'MacVST\Dehum\source'),
                  (Join-Path $plugins 'MacAU\Dehum'),
                  (Join-Path $plugins 'vdjplugin\vdj_dehum'))
    },
    @{
        From  = Join-Path $root 'foo_dsp_paraeq'
        Files = @('paraeq_core.h', 'paraeq_core.cpp')
        To    = @((Join-Path $plugins 'WinVST\ParaEQ'))
    },
    @{
        From  = Join-Path $plugins 'WinVST\Declick'
        Files = @('Declick.h', 'Declick.cpp', 'DeclickProc.cpp')
        To    = @((Join-Path $plugins 'LinuxVST\src\Declick'),
                  (Join-Path $plugins 'MacVST\Declick\source'))
    },
    @{
        From  = Join-Path $plugins 'WinVST\Dehum'
        Files = @('Dehum.h', 'Dehum.cpp', 'DehumProc.cpp')
        To    = @((Join-Path $plugins 'LinuxVST\src\Dehum'),
                  (Join-Path $plugins 'MacVST\Dehum\source'))
    }
)

$drifted = @()
$copied  = 0

foreach ($m in $mirrors) {
    foreach ($file in $m.Files) {
        $src = Join-Path $m.From $file
        if (-not (Test-Path $src)) { throw "canonical copy missing: $src" }
        $srcHash = (Get-FileHash -Algorithm SHA256 $src).Hash

        foreach ($dir in $m.To) {
            $dst = Join-Path $dir $file
            $rel = $dst.Substring($plugins.Length).TrimStart('\')

            if (Test-Path $dst) {
                if ((Get-FileHash -Algorithm SHA256 $dst).Hash -eq $srcHash) {
                    Write-Host ("  same     {0}" -f $rel) -ForegroundColor DarkGray
                    continue
                }
                $state = 'differs'
            } else {
                $state = 'missing'
            }

            if ($Check) {
                Write-Host ("  {0,-8} {1}" -f $state, $rel) -ForegroundColor Red
                $drifted += $rel
            } else {
                if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
                Copy-Item $src $dst -Force
                Write-Host ("  updated  {0}" -f $rel) -ForegroundColor Yellow
                $copied++
            }
        }
    }
}

if ($Check) {
    if ($drifted.Count -gt 0) {
        Write-Host ''
        Write-Warning (("{0} mirrored file(s) do not match their canonical copy. Run " +
            "scripts\sync_cores.ps1 to update them, or move the edit into the canonical " +
            "copy first if that is where it belongs.") -f $drifted.Count)
        exit 1
    }
    Write-Host "`nevery mirror matches" -ForegroundColor Green
    exit 0
}

Write-Host ("`n{0} file(s) updated" -f $copied) -ForegroundColor Green
exit 0
