# check_compat.ps1 - distribution compatibility gate (ASCII only on purpose)
#
# Why this exists: RLPlayer 1.0.0 was built with -march=znver5 (AVX-512) and
# dynamically linked the MSVC runtime. On a Ryzen 5900X (no AVX-512) the exe died
# with 0xC000001D, and on a clean Windows it would also ask for VCRUNTIME140.dll.
# This static screen catches the listed import/header/instruction mistakes.
# It does not prove the full CPU ISA baseline or old-Windows runtime behavior:
#   1) no VC++ runtime DLL imports (static CRT only)
#   2) PE subsystem version <= 6.1 (Windows 7 loader accepts it)
#   3) no AVX-512 instructions (zmm registers / EVEX masking)
#   4) no Windows 8+ / WinRT imports (Windows 7 PE audit)
#
# Rule 4 is adapted from DBX (Apache-2.0). The usual culprit is the WebView2
# loader: loaders >= 1.0.1054.31 import EventSetInformation, which Windows 7 and
# Server 2012 R2 do not provide, and the failure surfaces as the misleading
# "Could not find the WebView2 Runtime". The shippable loader is 1.0.902.49
# (official NuGet package Microsoft.Web.WebView2 1.0.902.49,
#  x64 WebView2Loader.dll sha256 fdf978ba706578b05967d7f0181f462147864a5aa74f36016a62cb3d3dbe6909).
# WebView2Loader.dll is audited even though it is not built here - it is copied
# in from third_party/webview2/x64, so a plain `ctest` can never catch it.
#
# Usage: powershell -File tools/check_compat.ps1 [-Dir build]
param([string]$Dir = "", [switch]$Recursive, [string]$Report = "")

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
if ($Dir -eq "") { $Dir = Join-Path $root 'build' }

# Locate LLVM binutils without hard-coding one vendor layout:
# PATH -> LLVM_HOME -> sibling toolchain/clang+llvm-* -> ROCm (the historical path).
function Find-Binutil([string]$exe) {
    $onPath = Get-Command $exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $cands = @()
    if ($env:LLVM_HOME) { $cands += (Join-Path $env:LLVM_HOME "bin\$exe") }
    $tcRoot = Join-Path (Split-Path -Parent $root) 'toolchain'
    if (Test-Path $tcRoot) {
        $cands += @(Get-ChildItem $tcRoot -Directory -Filter 'clang+llvm-*' -ErrorAction SilentlyContinue |
                    ForEach-Object { Join-Path $_.FullName "bin\$exe" })
    }
    $cands += "C:\Program Files\AMD\ROCm\7.2\bin\$exe"
    foreach ($c in $cands) { if ($c -and (Test-Path $c)) { return $c } }
    return $null
}

$ro = Find-Binutil 'llvm-readobj.exe'
$od = Find-Binutil 'llvm-objdump.exe'
foreach ($t in @($ro, $od)) {
    if (-not $t) { Write-Host "missing LLVM tool (llvm-readobj/llvm-objdump); install LLVM or ROCm, or set LLVM_HOME"; exit 2 }
}

# Offline PE screening does not prove x86-64-v2 dispatch or GPU execution.
# FULL requires both HIP and CUDA artifacts; real hardware scope is separate.
$files = @('RLPlayer.exe', 'rtaweb.exe', 'rtaplay.exe', 'asio_probe.exe', 'rtagpu.dll', 'WebView2Loader.dll', 'rtagpu_cuda.dll')
if($Recursive) {
    # Inspect every shipped PE, including unknown/new CLI tools. Vendor policy is
    # explicit and path-limited; it never exempts an unknown application binary.
    $files=@($files | Where-Object {$_ -ne 'rtaplay.exe'}) + @('tools\rtaplay.exe','ffmpeg.exe','ffprobe.exe','amdhip64_7.dll')
    $base=[IO.Path]::GetFullPath($Dir).TrimEnd('\')+'\'
    $discovered=@(Get-ChildItem -LiteralPath $Dir -Recurse -File | Where-Object {$_.Extension -in @('.exe','.dll')} | ForEach-Object {$_.FullName.Substring($base.Length)})
    $files=@($files+$discovered | Sort-Object -Unique)
}
$badRuntime = 'VCRUNTIME|MSVCP|api-ms-win-crt|ucrtbase'
$rows = @()

Write-Host "=== RLPlayer distribution compatibility check: $Dir ==="
foreach ($name in $files) {
    $f = Join-Path $Dir $name
    if (-not (Test-Path $f)) {
        $rows += [pscustomobject]@{ File = $name; Result = 'FAIL'; Detail = 'required file not found' }
        continue
    }
    if ((Get-Item -LiteralPath $f).Length -eq 0) {
        $rows += [pscustomobject]@{File=$name;Result='FAIL';Detail='empty required PE'}
        continue
    }
    $ErrorActionPreference='Continue'
    $imp = ((& $ro --coff-imports $f 2>&1) -join "`n")
    $impExit=$LASTEXITCODE
    $dlls = [regex]::Matches($imp, 'Name: (\S+)') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
    $rt = @($dlls | Where-Object { $_ -match $badRuntime })

    $hdr = ((& $ro --file-headers $f 2>&1) -join "`n")
    $hdrExit=$LASTEXITCODE
    $mv = [regex]::Match($hdr, 'MajorSubsystemVersion: (\d+)\s+MinorSubsystemVersion: (\d+)')
    $sub = 'unknown'
    $subOk = $false
    if ($mv.Success) {
        $sub = $mv.Groups[1].Value + '.' + $mv.Groups[2].Value
        $subOk = ([double]$sub -le 6.1)
    }

    $vendor=($name -in @('amdhip64_7.dll','ffmpeg.exe','ffprobe.exe','WebView2Loader.dll')) -or ($Recursive -and $name -like 'WebView2Runtime\*')
    if($vendor) {
        $ErrorActionPreference='Stop'
        $parsed=($impExit -eq 0 -and $hdrExit -eq 0 -and $imp -match 'Format: COFF' -and $hdr -match 'Format: COFF')
        $vendorVersion=[Diagnostics.FileVersionInfo]::GetVersionInfo($f).FileVersion
        $knownModern=($imp -match 'EventSetInformation|GetSystemTimePreciseAsFileTime|SetThreadDescription|api-ms-win-core-winrt')
        $rows += [pscustomobject]@{File=$name;Result=$(if($parsed){'PARSED_VENDOR_UNVERIFIED_OS_CPU'}else{'FAIL'});Detail="vendor parse=$parsed; file-version=$vendorVersion; subsystem=$sub; known-modern-import=$knownModern; imports=$($dlls -join ','); modern runtime has independent OS/CPU requirements; no legacy compatibility claim"}
        continue
    }
    $dis = ((& $od -d --section=.text $f 2>&1) -join "`n")
    $disExit=$LASTEXITCODE
    $ErrorActionPreference='Stop'
    $zmm = ([regex]::Matches($dis, 'zmm')).Count
    $mask = ([regex]::Matches($dis, '\{%k')).Count
    # AVX-512VL can encode YMM-only instructions without a k-mask or zmm name.
    # In 64-bit mode the 0x62 instruction prefix is EVEX, so inspect raw bytes too.
    $evex=([regex]::Matches($dis, '(?mi)^\s*[0-9a-f]+:\s+62\s+(?![^\r\n]*<unknown>)[^\r\n]+')).Count
    $avx512 = $zmm + $mask + $evex

    $dispatchOk=($avx512 -eq 0)
    if($avx512 -gt 0 -and $disExit -eq 0) {
        $dispatchReport=Join-Path $env:TEMP ('rlplayer-cpu-dispatch-'+[guid]::NewGuid().ToString('N')+'.json')
        & python (Join-Path $here 'check_cpu_dispatch.py') $f --llvm-dir (Split-Path -Parent $od) --report $dispatchReport
        $dispatchOk=($LASTEXITCODE -eq 0)
        if($Report -and (Test-Path -LiteralPath $dispatchReport)) {
            $saved=Join-Path (Split-Path -Parent $Report) ('cpu-dispatch-'+[guid]::NewGuid().ToString('N')+'.json')
            Copy-Item -LiteralPath $dispatchReport -Destination $saved
        }
    }
    $toolOk=($impExit -eq 0 -and $hdrExit -eq 0 -and $disExit -eq 0 -and $imp -match 'Format: COFF' -and $dis -match 'file format coff')
    $forbidden=($imp -match 'EventSetInformation|GetSystemTimePreciseAsFileTime|SetThreadDescription|api-ms-win-core-winrt')
    $ok = $toolOk -and (-not $forbidden) -and ($rt.Count -eq 0) -and $subOk -and $dispatchOk
    $crtText = if ($rt.Count -eq 0) { 'static' } else { 'dynamic:' + ($rt -join ',') }
    $rows += [pscustomobject]@{
        File   = $name
        Result = $(if ($ok) { 'PASS' } else { 'FAIL' })
        Detail = "tools=$toolOk; forbidden-import=$forbidden; crt=$crtText; subsystem=$sub; avx512-indicators=$avx512; evex=$evex; exact-crt-dispatch=$dispatchOk (static screen only; CPU/OS runtime compatibility requires separate hardware evidence)"
    }
}

if($Report){$rows | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Report -Encoding UTF8}
Write-Host ('Inspected PE entries: '+$rows.Count+'; recursive='+$Recursive)
Write-Host 'Scope: offline PE screen only. x86-64-v2 dispatch and old Windows execution require separate runtime evidence.'
$rows | Format-Table -AutoSize | Out-String | Write-Host
$fails = @($rows | Where-Object { $_.Result -eq 'FAIL' }).Count
Write-Host ("FAIL count: " + $fails)
exit $fails
