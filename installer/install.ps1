# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
    Installs, updates or removes the FFT Custom CHOP for TouchDesigner (TD_Custom_FFT).

.DESCRIPTION
    Puts the plugin where TouchDesigner looks for it - <Documents>\Derivative\Plugins\FFT - for the current
    user, no administrator rights needed:
      FFT.dll + the FFTW3 DLL + the license files     always
      Intel oneMKL (14 DLLs, 456 MB)                   with -OneMKL, or if you answer yes when asked; downloaded
                                                       from nuget.org and checked against a pinned SHA-256
      the example project(s)                           into <Documents>\TD_Custom_FFT\Examples (-NoExamples skips)

    Before it copies anything it checks the CPU (AVX2 + FMA), that TouchDesigner is closed, and that the
    Visual C++ runtime is new enough (it offers to install Microsoft's redistributable when it is not).

    Where the files come from, in this order: -Source (a folder or the release zip); the folder this script
    sits in, when it is the extracted TD_Custom_FFT-Windows.zip; otherwise the GitHub release (-Version, or
    the latest), verified against the release's SHA-256.

    Running it again updates in place; an installed oneMKL that is already the pinned version is kept without
    downloading it again.

.EXAMPLE
    irm https://raw.githubusercontent.com/NairoDorian/TD_Custom_FFT/main/installer/install.ps1 | iex
    Installs the latest release (asks whether to add oneMKL).

.EXAMPLE
    & ([scriptblock]::Create((irm https://raw.githubusercontent.com/NairoDorian/TD_Custom_FFT/main/installer/install.ps1))) -OneMKL -Yes
    Installs the latest release with oneMKL, without asking anything.

.EXAMPLE
    .\install.ps1 -Uninstall -Purge
    Removes the plugin, the examples and the FFT planning cache.

.NOTES
    Works in Windows PowerShell 5.1 and PowerShell 7. Part of https://github.com/NairoDorian/TD_Custom_FFT;
    the same checks as the Setup.exe installer (installer/TD_Custom_FFT.iss).
#>
[CmdletBinding()]
param(
    # Also install the Intel oneMKL backend (166 MB download from nuget.org). Kept if already current.
    [switch]$OneMKL,
    # Use this already-downloaded intelmkl.redist.win-x64 .nupkg instead of downloading it (offline installs;
    # it is still checked against the pinned SHA-256). Implies -OneMKL.
    [string]$OneMklPackage = '',
    # Remove an installed oneMKL backend.
    [switch]$RemoveOneMKL,
    # Do not copy the example project(s).
    [switch]$NoExamples,
    # Remove the plugin (and the examples) instead of installing it.
    [switch]$Uninstall,
    # With -Uninstall: also delete the FFT planning cache in %LOCALAPPDATA%\TD_Custom_FFT.
    [switch]$Purge,
    # Release tag to install, e.g. v2.14.0. Default: the latest release.
    [string]$Version = '',
    # Plugin folder. Default: <Documents>\Derivative\Plugins\FFT (or where Setup.exe installed it).
    [string]$Destination = '',
    # Install from this folder or TD_Custom_FFT-Windows.zip instead of downloading.
    [string]$Source = '',
    # Continue even if TouchDesigner is running (files in use are renamed aside; restart TouchDesigner after).
    [switch]$Force,
    # Never ask: take the default answer to every question (oneMKL: no unless -OneMKL; VC++ update: no).
    [switch]$Yes
)

function Install-TDCustomFFT {
    [CmdletBinding()]
    param([switch]$OneMKL, [string]$OneMklPackage = '', [switch]$RemoveOneMKL, [switch]$NoExamples, [switch]$Uninstall, [switch]$Purge,
          [string]$Version = '', [string]$Destination = '', [string]$Source = '', [switch]$Force, [switch]$Yes,
          [string]$ScriptDir = '')

    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue'   # Windows PowerShell 5.1's progress bar slows downloads ~10x

    # Stamped by installer/build_release.py into the copies a release ships; the repository copy keeps the
    # placeholders and then installs the latest release, verified against its SHA256SUMS.txt.
    $StampedTag = '@@RELEASE_TAG@@'
    $StampedZipSha256 = '@@ZIP_SHA256@@'

    $Repo = 'NairoDorian/TD_Custom_FFT'
    $ZipName = 'TD_Custom_FFT-Windows.zip'
    $InnoKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{6F2B8E1D-4C3A-4F57-9E0B-2D7A1C9F5E43}_is1'
    $VcRedistUrl = 'https://aka.ms/vc14/vc_redist.x64.exe'
    # Files of ours a plugin folder can hold. Nothing else in it is ever touched.
    $OwnedPatterns = @('FFT.dll', '*.dll.old*', 'libfftw3f-*.dll', 'FFT-LICENSE.txt', 'FFT-NOTICE.md',
                       'FFTW3-COPYING.txt', 'THIRD_PARTY_NOTICES.md')
    # Never needed beside FFT.dll; libiomp5md.dll would even abort TouchDesigner (two Intel OpenMP runtimes).
    $StrayFiles = @('libimalloc.dll', 'libiomp5md.dll', 'mkl_intel_thread.3.dll', 'mkl_tbb_thread.3.dll')

    function Step([string]$m) { Write-Host "==> $m" -ForegroundColor Cyan }
    function Note([string]$m) { Write-Host "    $m" }
    function Warn([string]$m) { Write-Host "WARNING: $m" -ForegroundColor Yellow }

    function Test-Interactive {
        if ($Yes) { return $false }
        try { return [Environment]::UserInteractive -and -not [Console]::IsInputRedirected } catch { return $false }
    }

    function Confirm-Choice([string]$Question, [bool]$Default) {
        if (-not (Test-Interactive)) { return $Default }
        $hint = '[y/N]'
        if ($Default) { $hint = '[Y/n]' }
        $a = Read-Host "$Question $hint"
        if ([string]::IsNullOrWhiteSpace($a)) { return $Default }
        return $a.Trim().ToLowerInvariant().StartsWith('y')
    }

    function Get-SystemDir {
        # A 32-bit PowerShell on 64-bit Windows sees SysWOW64 as System32; Sysnative is the real one.
        if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
            return (Join-Path $env:WINDIR 'Sysnative')
        }
        return (Join-Path $env:WINDIR 'System32')
    }

    function Get-DocumentsDir {
        # The real Documents folder, OneDrive or folder redirection included - the one TouchDesigner uses.
        $d = [Environment]::GetFolderPath('MyDocuments')
        if ([string]::IsNullOrEmpty($d)) { $d = Join-Path $env:USERPROFILE 'Documents' }
        return $d
    }

    # Relative to PowerShell's current location ($PWD), which [IO.Path]::GetFullPath does not know about.
    function Resolve-UserPath([string]$Path) {
        return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    }

    function Resolve-Destination {
        if ($Destination) { return (Resolve-UserPath $Destination) }
        if (Test-Path $InnoKey) {
            $loc = (Get-ItemProperty $InnoKey -ErrorAction SilentlyContinue).InstallLocation
            if ($loc) { return $loc.TrimEnd('\') }
        }
        return (Join-Path (Get-DocumentsDir) 'Derivative\Plugins\FFT')
    }

    function Test-Avx2Fma {
        if (-not ('TDCustomFFT.Cpu' -as [type])) {
            Add-Type -Namespace TDCustomFFT -Name Cpu -MemberDefinition `
                '[DllImport("kernel32.dll")] public static extern bool IsProcessorFeaturePresent(uint feature);'
        }
        $avx2 = [TDCustomFFT.Cpu]::IsProcessorFeaturePresent(40)   # PF_AVX2_INSTRUCTIONS_AVAILABLE
        # Windows has no FMA flag. PowerShell 7 (.NET) can ask the CPU; 5.1 cannot, and every AVX2 CPU has FMA3
        # (the plugin itself still reports a VM that masks it, instead of crashing).
        $fma = $true
        $t = 'System.Runtime.Intrinsics.X86.Fma' -as [type]
        if ($t) { $fma = [bool]$t::IsSupported }
        return ($avx2 -and $fma)
    }

    function Get-VcRuntimeVersion {
        $sys = Get-SystemDir
        foreach ($n in 'vcruntime140_1.dll', 'msvcp140_atomic_wait.dll') {
            if (-not (Test-Path (Join-Path $sys $n))) { return $null }
        }
        $f = Join-Path $sys 'msvcp140.dll'
        if (-not (Test-Path $f)) { return $null }
        $vi = (Get-Item $f).VersionInfo
        return New-Object Version $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart, $vi.FilePrivatePart
    }

    function Wait-TouchDesignerClosed {
        while ($true) {
            $running = @(Get-Process -Name 'TouchDesigner*' -ErrorAction SilentlyContinue)
            if ($running.Count -eq 0) { return }
            if ($Force) {
                Warn 'TouchDesigner is running (-Force): files in use are renamed aside; restart TouchDesigner afterwards.'
                return
            }
            if (-not (Test-Interactive)) {
                throw 'TouchDesigner is running and keeps the plugin files open. Close it and run this again (or pass -Force).'
            }
            Read-Host 'TouchDesigner is running and keeps the plugin files open. Save and close it, then press Enter to retry (Ctrl+C cancels)' | Out-Null
        }
    }

    function Invoke-Download([string]$Url, [string]$OutFile) {
        # TLS 1.2 for Windows PowerShell 5.1 on older Windows 10 builds (GitHub and nuget.org require it).
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $wc = New-Object Net.WebClient
        try { $wc.DownloadFile($Url, $OutFile) }
        catch {
            $why = $_.Exception.Message
            if ($_.Exception.InnerException) { $why = $_.Exception.InnerException.Message }
            $hint = ''
            if ($why -match '404') { $hint = ' (no such file: check -Version, or that the release is published)' }
            throw "Could not download $Url - $why$hint"
        }
        finally { $wc.Dispose() }
    }

    function Assert-Sha256([string]$File, [string]$Expected) {
        $got = (Get-FileHash -Algorithm SHA256 -LiteralPath $File).Hash
        if ($got -ne $Expected.ToUpperInvariant()) {
            throw "$([IO.Path]::GetFileName($File)): SHA-256 mismatch (expected $Expected, got $got). The download is damaged or not the published file; nothing was installed."
        }
    }

    # Removes a file; if it is in use (TouchDesigner running with -Force) renames it aside instead, which Windows
    # allows for a loaded DLL. The next run cleans the *.old* names up.
    function Remove-OrRename([string]$Path) {
        if (-not (Test-Path -LiteralPath $Path)) { return }
        try { Remove-Item -LiteralPath $Path -Force }
        catch {
            for ($i = 1; $i -lt 100; $i++) {
                $aside = "$Path.old$i"
                if (-not (Test-Path -LiteralPath $aside)) { Rename-Item -LiteralPath $Path -NewName ([IO.Path]::GetFileName($aside)); return }
            }
            throw
        }
    }

    function Expand-ZipEntries([string]$Zip, [string]$OutDir, [hashtable]$Map) {
        # Extracts only the listed entries (archive path -> output name). .NET instead of Expand-Archive, which
        # in Windows PowerShell 5.1 refuses any extension but .zip (a .nupkg is a zip).
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [IO.Compression.ZipFile]::OpenRead($Zip)
        try {
            $found = @{}
            foreach ($e in $archive.Entries) {
                $key = $e.FullName.Replace('\', '/')
                if ($Map.ContainsKey($key)) {
                    $out = Join-Path $OutDir $Map[$key]
                    $dir = Split-Path $out -Parent
                    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
                    [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $out, $true)
                    $found[$key] = $true
                }
            }
            $missing = @($Map.Keys | Where-Object { -not $found.ContainsKey($_) })
            if ($missing.Count) { throw "$([IO.Path]::GetFileName($Zip)) does not contain: $($missing -join ', ')" }
        } finally { $archive.Dispose() }
    }

    function Expand-WholeZip([string]$Zip, [string]$OutDir) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::ExtractToDirectory($Zip, $OutDir)
    }

    # ---- the package: a folder holding manifest.json, FFT\ and Examples\ -----------------------------------
    function Get-Package([string]$Tmp) {
        if ($Source) {
            $src = Resolve-UserPath $Source
            if (Test-Path -LiteralPath $src -PathType Leaf) {
                Step "Unpacking $src"
                $dir = Join-Path $Tmp 'package'
                Expand-WholeZip $src $dir
                return $dir
            }
            return $src
        }
        if ($ScriptDir -and (Test-Path (Join-Path $ScriptDir 'manifest.json')) -and (Test-Path (Join-Path $ScriptDir 'FFT\FFT.dll'))) {
            return $ScriptDir   # run from the extracted release zip
        }
        $tag = $Version
        if ($tag -and -not $tag.StartsWith('v')) { $tag = "v$tag" }
        if (-not $tag -and -not $StampedTag.StartsWith('@@')) { $tag = $StampedTag }
        if ($tag) { $base = "https://github.com/$Repo/releases/download/$tag" }
        else { $base = "https://github.com/$Repo/releases/latest/download" }
        $zip = Join-Path $Tmp $ZipName
        Step "Downloading $ZipName ($(if ($tag) { $tag } else { 'latest release' }))"
        Invoke-Download "$base/$ZipName" $zip
        if ($tag -and $tag -eq $StampedTag -and -not $StampedZipSha256.StartsWith('@@')) {
            $expected = $StampedZipSha256
        } else {
            $sums = Join-Path $Tmp 'SHA256SUMS.txt'
            Invoke-Download "$base/SHA256SUMS.txt" $sums
            $pattern = "^([0-9a-fA-F]{64})\s+\*?$([regex]::Escape($ZipName))$"
            $line = Get-Content $sums | Where-Object { $_ -match $pattern } | Select-Object -First 1
            if (-not $line -or -not ($line -match $pattern)) { throw "SHA256SUMS.txt of that release does not list $ZipName" }
            $expected = $Matches[1]
        }
        Assert-Sha256 $zip $expected
        Note 'SHA-256 verified'
        $dir = Join-Path $Tmp 'package'
        Expand-WholeZip $zip $dir
        return $dir
    }

    # ---- oneMKL ------------------------------------------------------------------------------------------
    function Test-OneMklCurrent([string]$Dir, $Pin) {
        foreach ($d in $Pin.dlls) { if (-not (Test-Path (Join-Path $Dir $d))) { return $false } }
        $marker = Join-Path $Dir "oneMKL-licenses\$($Pin.marker)"
        if ((Test-Path $marker) -and ((Get-Content $marker -Raw).Trim() -eq "$($Pin.package) $($Pin.package_version)")) { return $true }
        $vi = (Get-Item (Join-Path $Dir 'mkl_rt.3.dll')).VersionInfo
        return ("{0}.{1}.{2}.{3}" -f $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart, $vi.FilePrivatePart) -eq $Pin.file_version
    }

    function Remove-OneMkl([string]$Dir) {
        Get-ChildItem -LiteralPath $Dir -Filter 'mkl_*.dll' -File -ErrorAction SilentlyContinue | ForEach-Object { Remove-OrRename $_.FullName }
        $lic = Join-Path $Dir 'oneMKL-licenses'
        if (Test-Path $lic) { Remove-Item -LiteralPath $lic -Recurse -Force }
    }

    function Install-OneMkl([string]$Dir, $Pin, [string]$Tmp) {
        $pkg = Join-Path $Tmp 'onemkl.nupkg'
        if ($OneMklPackage) {
            Step "Using the oneMKL package $OneMklPackage"
            Copy-Item -LiteralPath (Resolve-UserPath $OneMklPackage) -Destination $pkg
        } else {
            $mb = [math]::Round($Pin.size / 1MB)
            Step "Downloading Intel oneMKL $($Pin.package_version) from nuget.org ($mb MB)"
            Invoke-Download $Pin.url $pkg
        }
        Assert-Sha256 $pkg $Pin.sha256
        Note 'SHA-256 verified'
        $map = @{}
        foreach ($d in $Pin.dlls) { $map["$($Pin.dll_dir)/$d"] = $d }
        foreach ($p in $Pin.licenses.PSObject.Properties) { $map[$p.Name] = "oneMKL-licenses\$($p.Value)" }
        $out = Join-Path $Tmp 'onemkl'
        Expand-ZipEntries $pkg $out $map
        Remove-Item -LiteralPath $pkg -Force
        Remove-OneMkl $Dir
        Copy-Item -Path (Join-Path $out '*') -Destination $Dir -Recurse -Force
        Set-Content -LiteralPath (Join-Path $Dir "oneMKL-licenses\$($Pin.marker)") -Value "$($Pin.package) $($Pin.package_version)" -Encoding Ascii
        Note "$($Pin.dlls.Count) DLLs installed (Intel Simplified Software License: oneMKL-licenses\license.txt)"
    }

    # ---- uninstall ---------------------------------------------------------------------------------------
    function Invoke-Uninstall {
        $dest = Resolve-Destination
        Wait-TouchDesignerClosed
        if (Test-Path $InnoKey) {
            $u = (Get-ItemProperty $InnoKey).UninstallString
            if ($u) {
                $exe = $u.Trim('"')
                if (Test-Path -LiteralPath $exe) {
                    Step 'Running the Setup.exe uninstaller'
                    Start-Process -FilePath $exe -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait
                }
            }
        }
        if (Test-Path -LiteralPath $dest) {
            Step "Removing the plugin from $dest"
            foreach ($pat in $OwnedPatterns + $StrayFiles) {
                Get-ChildItem -LiteralPath $dest -Filter $pat -File -ErrorAction SilentlyContinue | ForEach-Object { Remove-OrRename $_.FullName }
            }
            Remove-OneMkl $dest
            if (-not (Get-ChildItem -LiteralPath $dest -Force)) { Remove-Item -LiteralPath $dest -Force }
            else { Note 'The folder still holds files that are not ours (or are in use); it was left in place.' }
        } else {
            Note "Nothing installed in $dest"
        }
        $ex = Join-Path (Get-DocumentsDir) 'TD_Custom_FFT\Examples'
        if (Test-Path $ex) {
            Step "Removing the examples from $ex"
            Get-ChildItem -LiteralPath $ex -Filter '*.toe' -File | Remove-Item -Force
            foreach ($d in $ex, (Split-Path $ex -Parent)) {
                if ((Test-Path $d) -and -not (Get-ChildItem -LiteralPath $d -Force)) { Remove-Item -LiteralPath $d -Force }
            }
        }
        if ($Purge) {
            $cache = Join-Path $env:LOCALAPPDATA 'TD_Custom_FFT'
            Get-ChildItem -LiteralPath $cache -Filter 'fftwf_wisdom*.txt' -File -ErrorAction SilentlyContinue | Remove-Item -Force
            Note 'FFT planning cache deleted'
        }
        Step 'Done.'
    }

    # ---- install -----------------------------------------------------------------------------------------
    if ($Uninstall) { Invoke-Uninstall; return }
    if ($OneMklPackage) { $OneMKL = [switch]::Present }

    Write-Host ''
    Write-Host 'TD_Custom_FFT - FFT Custom CHOP for TouchDesigner' -ForegroundColor White
    if (-not [Environment]::Is64BitOperatingSystem) { throw 'A 64-bit Windows is required (TouchDesigner is 64-bit only).' }
    if (-not (Test-Avx2Fma)) {
        throw 'This processor lacks AVX2/FMA, which the FFT Custom CHOP requires (Intel since 2013, AMD since 2015). The operator would only ever show an error here, so nothing was installed.'
    }

    $dest = Resolve-Destination
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('TD_Custom_FFT-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force $tmp | Out-Null
    try {
        $pkg = Get-Package $tmp
        $manifestPath = Join-Path $pkg 'manifest.json'
        if (-not (Test-Path $manifestPath)) { throw "$pkg is not a TD_Custom_FFT package (no manifest.json)" }
        $m = Get-Content $manifestPath -Raw | ConvertFrom-Json
        Note "Package: $($m.name) $($m.tag) (commit $($m.commit))"
        Note "Plugin folder: $dest"

        # Visual C++ runtime: at least the one FFT.dll was compiled against (Microsoft's rule).
        $need = [Version]$m.min_vc_runtime
        $have = Get-VcRuntimeVersion
        if (-not $have -or ($have.Major -lt $need.Major) -or ($have.Major -eq $need.Major -and $have.Minor -lt $need.Minor)) {
            $haveText = 'not installed'
            if ($have) { $haveText = $have.ToString() }
            Warn "The Visual C++ runtime here is $haveText; the plugin needs $need or newer, otherwise TouchDesigner may refuse to load it or crash."
            if (Confirm-Choice 'Download and install the current Microsoft Visual C++ Redistributable (x64) now? Windows will ask for administrator rights.' $false) {
                $vc = Join-Path $tmp 'vc_redist.x64.exe'
                Step 'Downloading the Visual C++ Redistributable from Microsoft'
                Invoke-Download $VcRedistUrl $vc
                $sig = Get-AuthenticodeSignature -LiteralPath $vc
                if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
                    throw "vc_redist.x64.exe is not validly signed by Microsoft ($($sig.Status)); not running it."
                }
                try {
                    $p = Start-Process -FilePath $vc -ArgumentList '/install', '/passive', '/norestart' -Verb RunAs -Wait -PassThru
                    Note "vc_redist exit code $($p.ExitCode) (0 installed, 1638 already newer, 3010 reboot needed)"
                } catch { Warn "The Visual C++ Redistributable was not installed: $($_.Exception.Message)" }
            } else {
                Note "Install it later from $VcRedistUrl if TouchDesigner cannot load the plugin."
            }
        }

        Wait-TouchDesignerClosed

        Step "Installing the plugin into $dest"
        New-Item -ItemType Directory -Force $dest | Out-Null
        foreach ($pat in $OwnedPatterns + $StrayFiles) {
            Get-ChildItem -LiteralPath $dest -Filter $pat -File -ErrorAction SilentlyContinue | ForEach-Object { Remove-OrRename $_.FullName }
        }
        foreach ($f in $m.plugin_files) {
            Copy-Item -LiteralPath (Join-Path $pkg "FFT\$f") -Destination (Join-Path $dest $f) -Force
        }
        # Downloaded through a browser, the files carry the internet zone mark; the plugin does not need it.
        Get-ChildItem -LiteralPath $dest -File | Unblock-File -ErrorAction SilentlyContinue
        # Loose copies at the root of the Plugins folder (older manual installs) would register the operator twice.
        $pluginsRoot = Join-Path (Get-DocumentsDir) 'Derivative\Plugins'
        foreach ($loose in 'FFT.dll', 'libfftw3f-3.3.11-avx2.dll') {
            $lp = Join-Path $pluginsRoot $loose
            if ((Test-Path -LiteralPath $lp) -and ($pluginsRoot -ne $dest)) { Remove-OrRename $lp; Note "Removed a loose $loose from $pluginsRoot" }
        }
        Note "$(@($m.plugin_files).Count) files"

        $pin = $m.onemkl
        $mklPresent = Test-Path (Join-Path $dest 'mkl_rt.3.dll')
        $mklCurrent = $mklPresent -and (Test-OneMklCurrent $dest $pin)
        if ($RemoveOneMKL) {
            if ($mklPresent) { Step 'Removing Intel oneMKL'; Remove-OneMkl $dest }
        } elseif ($OneMKL) {
            if ($mklCurrent) { Note "Intel oneMKL $($pin.package_version) already installed: kept" }
            else { Install-OneMkl $dest $pin $tmp }
        } elseif (-not $mklPresent) {
            if (Confirm-Choice "Also install the optional Intel oneMKL backend (a faster FFT; $([math]::Round($pin.size / 1MB)) MB download from nuget.org, $([math]::Round($pin.installed_size / 1MB)) MB on disk)?" $false) {
                Install-OneMkl $dest $pin $tmp
            }
        } elseif (-not $mklCurrent) {
            Warn "A different oneMKL is installed beside FFT.dll. Run again with -OneMKL to replace it with the tested $($pin.package_version), or -RemoveOneMKL."
        } else {
            Note "Intel oneMKL $($pin.package_version): kept"
        }

        if (-not $NoExamples -and @($m.examples).Count) {
            $ex = Join-Path (Get-DocumentsDir) 'TD_Custom_FFT\Examples'
            Step "Copying the example project(s) into $ex"
            New-Item -ItemType Directory -Force $ex | Out-Null
            foreach ($e in $m.examples) { Copy-Item -LiteralPath (Join-Path $pkg "Examples\$e") -Destination (Join-Path $ex $e) -Force }
            Get-ChildItem -LiteralPath $ex -File | Unblock-File -ErrorAction SilentlyContinue
        }

        Write-Host ''
        Step "Installed $($m.name) $($m.tag)."
        Note 'Start TouchDesigner, press Tab and create CHOP > FFT Custom. If TouchDesigner asks whether to load the plugin, allow it.'
        Note 'Update: run this again. Remove: add -Uninstall.'
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# Run as a file (Install.cmd, or .\install.ps1) a failure sets the exit code; run through `irm | iex` it must not
# call exit, which would close the user's PowerShell window.
$__tdfftAsFile = [bool]$MyInvocation.MyCommand.Path
$__tdfftArgs = @{}
foreach ($__k in $PSBoundParameters.Keys) { $__tdfftArgs[$__k] = $PSBoundParameters[$__k] }
if ($__tdfftAsFile) { $__tdfftArgs['ScriptDir'] = Split-Path -Parent $MyInvocation.MyCommand.Path }
try {
    Install-TDCustomFFT @__tdfftArgs
} catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    if ($__tdfftAsFile) { exit 1 }
}
