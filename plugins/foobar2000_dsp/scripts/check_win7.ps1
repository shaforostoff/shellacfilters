<#
.SYNOPSIS
    Checks that a built component can actually be loaded by Windows 7.

.DESCRIPTION
    Whether a DLL loads on Windows 7 comes down to three things in the PE
    image, all of which can be read off the finished file:

      1. The minimum OS / subsystem version in the optional header. The loader
         refuses an image that asks for more than the running OS provides.
      2. The list of DLLs it imports. Windows 7 has no api-ms-win-* API sets,
         no combase.dll and no shcore.dll, and the 14.40+ Visual C++
         redistributable (vcruntime140.dll, msvcp140.dll, ucrtbase.dll) no
         longer installs there - so a dynamic CRT is a dead end.
      3. The individual functions it imports. A single Windows 8 or Windows 10
         export in the import table stops the DLL from loading, with an error
         message that says nothing useful about which one.

    This does NOT prove the component works, only that it can be loaded:
    functions resolved at runtime with GetProcAddress are invisible here, and
    so is the instruction set the code was compiled for (see FOO_DSP_ARCH -
    an AVX build fails at the first AVX instruction, not at load time).

.PARAMETER Path
    Files or directories to check: .dll files, .fb2k-component / .zip
    archives, or directories (searched recursively for *.dll). Defaults to
    build\_package, i.e. whatever the last build_release.ps1 run staged.

.PARAMETER Strict
    Treat warnings as failures.

.EXAMPLE
    .\scripts\check_win7.ps1

.EXAMPLE
    .\scripts\check_win7.ps1 ..\dist\foo_dsp_declick-1.0.1.fb2k-component
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]] $Path,
    [switch]   $Strict
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot

# Windows 7 is 6.1. An image whose subsystem version is higher is rejected by
# the loader before a single byte of it runs.
$maxOsVersion = [version] '6.1'

# --- what Windows 7 does not have -----------------------------------------
$hostileModules = @(
    @{ Pattern = '^api-ms-win-';                      Level = 'fail'; Reason = 'API set DLLs were introduced in Windows 8' }
    @{ Pattern = '^ext-ms-';                          Level = 'fail'; Reason = 'API set DLLs were introduced in Windows 8' }
    @{ Pattern = '^combase\.dll$';                    Level = 'fail'; Reason = 'introduced in Windows 8' }
    @{ Pattern = '^shcore\.dll$';                     Level = 'fail'; Reason = 'introduced in Windows 8' }
    @{ Pattern = '^windows\.storage\.dll$';           Level = 'fail'; Reason = 'introduced in Windows 8' }
    @{ Pattern = '^ucrtbase(d)?\.dll$';               Level = 'fail'; Reason = 'dynamic UCRT; configure with -DFOO_DSP_STATIC_CRT=ON' }
    @{ Pattern = '^vcruntime\d+(_\d+)?(d)?\.dll$';    Level = 'fail'; Reason = 'the 14.40+ VC++ redistributable does not install on Windows 7; configure with -DFOO_DSP_STATIC_CRT=ON' }
    @{ Pattern = '^msvcp\d+(_\d+)?(d)?\.dll$';        Level = 'fail'; Reason = 'the 14.40+ VC++ redistributable does not install on Windows 7; configure with -DFOO_DSP_STATIC_CRT=ON' }
    @{ Pattern = '^concrt\d+(d)?\.dll$';              Level = 'fail'; Reason = 'part of the VC++ redistributable; configure with -DFOO_DSP_STATIC_CRT=ON' }
    @{ Pattern = '^msvcr\d+(d)?\.dll$';               Level = 'warn'; Reason = 'pre-UCRT runtime; needs its own redistributable' }
)

# Imports that do not resolve on Windows 7. Not exhaustive for all of Windows,
# but covers what a C++ toolchain, the STL and this project can pull in.
$hostileImports = @{
    # --- synchronisation: the usual way a modern STL breaks Windows 7 ---
    'WaitOnAddress'                        = 'Windows 8'
    'WakeByAddressSingle'                  = 'Windows 8'
    'WakeByAddressAll'                     = 'Windows 8'
    # --- time ---
    'GetSystemTimePreciseAsFileTime'       = 'Windows 8'
    'QueryInterruptTimePrecise'            = 'Windows 10'
    'QueryUnbiasedInterruptTimePrecise'    = 'Windows 10'
    # --- threads / processes ---
    'SetThreadDescription'                 = 'Windows 10 1607'
    'GetThreadDescription'                 = 'Windows 10 1607'
    'SetThreadInformation'                 = 'Windows 8'
    'GetThreadInformation'                 = 'Windows 8'
    'SetProcessMitigationPolicy'           = 'Windows 8'
    'GetProcessMitigationPolicy'           = 'Windows 8'
    'RaiseFailFastException'               = 'Windows 8'
    'IsWow64Process2'                      = 'Windows 10 1511'
    'SetProcessValidCallTargets'           = 'Windows 10'
    # --- UCRT / CRT odds and ends ---
    'ProcessPrng'                          = 'Windows 10'
    'GetTempPath2W'                        = 'Windows 10 20H1'
    'GetTempPath2A'                        = 'Windows 10 20H1'
    'RtlGetDeviceFamilyInfoEnum'           = 'Windows 10'
    'AppPolicyGetProcessTerminationMethod' = 'Windows 10'
    'AppPolicyGetThreadInitializationType' = 'Windows 10'
    'AppPolicyGetShowDeveloperDiagnostic'  = 'Windows 10'
    'AppPolicyGetWindowingModel'           = 'Windows 10'
    'AppPolicyGetMediaFoundationCodecLoading' = 'Windows 10'
    'AppPolicyGetClrCompat'                = 'Windows 10'
    'AppPolicyGetCreateFileAccess'         = 'Windows 10'
    'AppPolicyGetLifecycleManagement'      = 'Windows 10'
    # --- files / memory ---
    'CreateFile2'                          = 'Windows 8'
    'GetOverlappedResultEx'                = 'Windows 8'
    'CreateFileMappingFromApp'             = 'Windows 8'
    'OpenFileMappingFromApp'               = 'Windows 8'
    'MapViewOfFileFromApp'                 = 'Windows 8'
    'VirtualAllocFromApp'                  = 'Windows 8'
    'VirtualAlloc2'                        = 'Windows 10'
    'MapViewOfFile3'                       = 'Windows 10'
    'DiscardVirtualMemory'                 = 'Windows 8.1'
    'OfferVirtualMemory'                   = 'Windows 8.1'
    'ReclaimVirtualMemory'                 = 'Windows 8.1'
    'GetFileVersionInfoExW'                = 'Windows 8'
    'GetFileVersionInfoSizeExW'            = 'Windows 8'
    # --- path helpers (api-ms-win-core-path, but also worth naming) ---
    'PathCchAppend'                        = 'Windows 8'
    'PathCchCombine'                       = 'Windows 8'
    'PathCchCombineEx'                     = 'Windows 8'
    'PathCchCanonicalizeEx'                = 'Windows 8'
    'PathCchRemoveFileSpec'                = 'Windows 8'
    # --- packaging / WinRT ---
    'LoadPackagedLibrary'                  = 'Windows 8'
    'GetCurrentPackageFullName'            = 'Windows 8'
    'GetPackageFullName'                   = 'Windows 8'
    'GetPackageFamilyName'                 = 'Windows 8'
    'RoInitialize'                         = 'Windows 8'
    'RoActivateInstance'                   = 'Windows 8'
    # --- per-monitor DPI, which a preferences dialog can wander into ---
    'GetDpiForWindow'                      = 'Windows 10 1607'
    'GetDpiForSystem'                      = 'Windows 10 1607'
    'GetSystemMetricsForDpi'               = 'Windows 10 1607'
    'AdjustWindowRectExForDpi'             = 'Windows 10 1607'
    'EnableNonClientDpiScaling'            = 'Windows 10 1607'
    'SetThreadDpiAwarenessContext'         = 'Windows 10 1607'
    'GetThreadDpiAwarenessContext'         = 'Windows 10 1607'
    'SetProcessDpiAwarenessContext'        = 'Windows 10 1703'
    'AreDpiAwarenessContextsEqual'         = 'Windows 10 1607'
    'GetDpiForMonitor'                     = 'Windows 8.1'
    'SetProcessDpiAwareness'               = 'Windows 8.1'
}

# Present on Windows 7 SP1, but only once an update has been applied. Worth
# knowing about; not worth failing over.
$suspectImports = @{
    'SetDefaultDllDirectories' = 'Windows 8; on Windows 7 SP1 only with KB2533623'
    'AddDllDirectory'          = 'Windows 8; on Windows 7 SP1 only with KB2533623'
    'RemoveDllDirectory'       = 'Windows 8; on Windows 7 SP1 only with KB2533623'
}

# --- dumpbin ---------------------------------------------------------------
function Find-Dumpbin {
    $candidates = New-Object System.Collections.Generic.List[string]

    if ($env:VCToolsInstallDir) {
        foreach ($hit in (Get-ChildItem -Path $env:VCToolsInstallDir -Filter dumpbin.exe -Recurse -ErrorAction SilentlyContinue)) {
            $candidates.Add($hit.FullName)
        }
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        foreach ($install in (& $vswhere -all -products * -property installationPath)) {
            $tools = Join-Path $install 'VC\Tools\MSVC'
            if (-not (Test-Path $tools)) { continue }
            foreach ($v in (Get-ChildItem $tools -Directory | Sort-Object Name -Descending)) {
                foreach ($host_ in @('Hostx64\x64', 'Hostx86\x86')) {
                    $exe = Join-Path $v.FullName "bin\$host_\dumpbin.exe"
                    if (Test-Path $exe) { $candidates.Add($exe) }
                }
            }
        }
    }

    $onPath = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($onPath) { $candidates.Add($onPath.Source) }

    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    throw 'dumpbin.exe not found. Run this from a Visual Studio developer prompt, or install the MSVC build tools.'
}

$dumpbin = Find-Dumpbin

function Invoke-Dumpbin([string] $file, [string] $switch) {
    # Full path, because dumpbin loads its own DLLs from its own directory.
    $out = & $dumpbin "-$switch" "-nologo" $file 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin -$switch failed for ${file}:`n$($out -join [Environment]::NewLine)"
    }
    return $out
}

function Get-PeVersions([string] $file) {
    $lines = Invoke-Dumpbin $file 'headers'
    $info = [ordered] @{ Machine = '?'; Linker = '?'; OsVersion = $null; SubsystemVersion = $null }
    foreach ($line in $lines) {
        if ($line -match '^\s*([0-9A-F]+)\s+machine\s+\((.+)\)')        { $info.Machine = $Matches[2] }
        elseif ($line -match '^\s*([\d]+\.[\d]+) linker version')        { $info.Linker = $Matches[1] }
        elseif ($line -match '^\s*([\d]+\.[\d]+) operating system version') { $info.OsVersion = [version] $Matches[1] }
        elseif ($line -match '^\s*([\d]+\.[\d]+) subsystem version')      { $info.SubsystemVersion = [version] $Matches[1] }
    }
    return $info
}

function Get-PeImports([string] $file) {
    $lines = Invoke-Dumpbin $file 'imports'
    $imports = [ordered] @{}
    $current = $null
    foreach ($line in $lines) {
        if ($line -match '^\s{4}(\S+\.[Dd][Ll][Ll])\s*$') {
            $current = $Matches[1]
            if (-not $imports.Contains($current)) {
                $imports[$current] = New-Object System.Collections.Generic.List[string]
            }
            continue
        }
        # "<hint> <name>" - the Summary section's "<size> .text" cannot match,
        # a leading dot is not a valid identifier start.
        if ($current -and $line -match '^\s+[0-9A-Fa-f]+\s+([A-Za-z_@?][\w@?$]*)\s*$') {
            $imports[$current].Add($Matches[1])
        }
    }
    return $imports
}

# --- collect the files to look at ------------------------------------------
if (-not $Path) { $Path = @(Join-Path $root 'build\_package') }

$temp = $null
$files = New-Object System.Collections.Generic.List[string]
foreach ($p in $Path) {
    $resolved = Resolve-Path -LiteralPath $p -ErrorAction Stop
    foreach ($r in $resolved) {
        $item = Get-Item -LiteralPath $r.Path
        if ($item.PSIsContainer) {
            foreach ($dll in (Get-ChildItem -LiteralPath $item.FullName -Filter *.dll -Recurse)) {
                $files.Add($dll.FullName)
            }
        }
        elseif ($item.Extension -in @('.fb2k-component', '.zip')) {
            if (-not $temp) {
                $temp = Join-Path ([System.IO.Path]::GetTempPath()) "check_win7_$PID"
                New-Item -ItemType Directory -Force $temp | Out-Null
            }
            $dest = Join-Path $temp $item.BaseName
            New-Item -ItemType Directory -Force $dest | Out-Null
            # cmake -E tar reads zips regardless of the extension; Expand-Archive
            # insists on .zip.
            & cmake -E chdir $dest cmake -E tar xf $item.FullName
            if ($LASTEXITCODE -ne 0) { throw "Could not unpack $($item.FullName)" }
            foreach ($dll in (Get-ChildItem -LiteralPath $dest -Filter *.dll -Recurse)) {
                $files.Add($dll.FullName)
            }
        }
        else {
            $files.Add($item.FullName)
        }
    }
}

if (-not $files) { throw "No DLLs found in: $($Path -join ', ')" }

# --- check -----------------------------------------------------------------
$failures = 0
$warnings = 0

try {
    foreach ($file in $files) {
        $shown = $file
        if ($file.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
            $shown = $file.Substring($root.Length).TrimStart('\')
        }
        Write-Host "`n$shown" -ForegroundColor Cyan

        $v = Get-PeVersions $file
        Write-Host ("  {0}, linker {1}, minimum OS {2}, subsystem {3}" -f
            $v.Machine, $v.Linker,
            $(if ($v.OsVersion) { $v.OsVersion } else { '?' }),
            $(if ($v.SubsystemVersion) { $v.SubsystemVersion } else { '?' })) -ForegroundColor DarkGray

        $fileFailures = New-Object System.Collections.Generic.List[string]
        $fileWarnings = New-Object System.Collections.Generic.List[string]

        foreach ($field in @('OsVersion', 'SubsystemVersion')) {
            $value = $v.$field
            if (-not $value) {
                $fileWarnings.Add("could not read the $field field out of the PE header")
            }
            elseif ($value -gt $maxOsVersion) {
                $fileFailures.Add("$field is $value, Windows 7 is $maxOsVersion - the loader will reject this image")
            }
        }

        $imports = Get-PeImports $file
        foreach ($module in $imports.Keys) {
            foreach ($rule in $hostileModules) {
                if ($module -match $rule.Pattern) {
                    $text = "imports $module - $($rule.Reason)"
                    if ($rule.Level -eq 'fail') { $fileFailures.Add($text) } else { $fileWarnings.Add($text) }
                }
            }
            foreach ($fn in $imports[$module]) {
                if ($hostileImports.ContainsKey($fn)) {
                    $fileFailures.Add("imports $module!$fn - $($hostileImports[$fn])")
                }
                elseif ($suspectImports.ContainsKey($fn)) {
                    $fileWarnings.Add("imports $module!$fn - $($suspectImports[$fn])")
                }
            }
        }

        $modules = @($imports.Keys | Sort-Object)
        $moduleSummary = ($modules | Select-Object -First 12) -join ', '
        if ($modules.Count -gt 12) {
            $moduleSummary += " (+$($modules.Count - 12) more)"
        }
        Write-Host "  imports: $moduleSummary" -ForegroundColor DarkGray

        foreach ($w in $fileWarnings) { Write-Host "  WARN  $w" -ForegroundColor Yellow }
        foreach ($f in $fileFailures) { Write-Host "  FAIL  $f" -ForegroundColor Red }

        $warnings += $fileWarnings.Count
        $failures += $fileFailures.Count
        if ($Strict) { $failures += $fileWarnings.Count }

        if (-not $fileFailures -and -not $fileWarnings) {
            Write-Host "  OK    nothing in this image needs anything newer than Windows 7" -ForegroundColor Green
        }
    }
}
finally {
    if ($temp -and (Test-Path $temp)) { Remove-Item -Recurse -Force $temp }
}

Write-Host ''
if ($failures -gt 0) {
    Write-Host "Windows 7 check FAILED: $failures problem(s), $warnings warning(s)." -ForegroundColor Red
    exit 1
}
if ($warnings -gt 0) {
    Write-Host "Windows 7 check passed with $warnings warning(s)." -ForegroundColor Yellow
}
else {
    Write-Host "Windows 7 check passed for $($files.Count) file(s)." -ForegroundColor Green
}
Write-Host "Load-time only: runtime GetProcAddress calls and the instruction set baseline are not covered." -ForegroundColor DarkGray
exit 0
