# check_compat.ps1 - distribution compatibility gate (ASCII only on purpose)
#
# Why this exists: RLPlayer 1.0.0 was built with -march=znver5 (AVX-512) and
# dynamically linked the MSVC runtime. On a Ryzen 5900X (no AVX-512) the exe died
# with 0xC000001D, and on a clean Windows it would also ask for VCRUNTIME140.dll.
# This script makes those mistakes impossible to ship:
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
param([string]$Dir = "")

$ErrorActionPreference = 'Continue'
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

# rtagpu_cuda.dll: the CUDA backend (skipped when the machine has no CUDA toolchain).
# WebView2Loader.dll: shipped verbatim from third_party, hence audited here.
$files = @('RLPlayer.exe', 'rtaweb.exe', 'rtaplay.exe', 'asio_probe.exe',
           'rtagpu.dll', 'rtagpu_cuda.dll', 'WebView2Loader.dll')
$badRuntime = 'VCRUNTIME|MSVCP|api-ms-win-crt|ucrtbase'

# Imports that exist only on Windows 8+ / WinRT. A binary importing any of these
# cannot load on Windows 7 even though its subsystem version says 6.1.
$forbidden = [ordered]@{
    'combase.dll'           = 'Win8+ only; use OLE32'
    'api-ms-win-core-winrt' = 'WinRT API sets unavailable on Win7'
    'CoIncrementMTAUsage'   = 'unavailable on Win7'
    'EventSetInformation'   = 'unavailable on Win7; WebView2 loader must be <= 1.0.902.49'
}

$rows = @()

Write-Host "=== RLPlayer distribution compatibility check: $Dir ==="
foreach ($name in $files) {
    $f = Join-Path $Dir $name
    if (-not (Test-Path $f)) {
        $rows += [pscustomobject]@{ File = $name; Result = 'SKIP'; Detail = 'not found' }
        continue
    }
    $imp = ((& $ro --coff-imports $f 2>&1) -join "`n")
    $dlls = [regex]::Matches($imp, 'Name: (\S+)') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
    $rt = @($dlls | Where-Object { $_ -match $badRuntime })

    $hdr = ((& $ro --file-headers $f 2>&1) -join "`n")
    $mv = [regex]::Match($hdr, 'MajorSubsystemVersion: (\d+)\s+MinorSubsystemVersion: (\d+)')
    $sub = 'unknown'
    $subOk = $false
    if ($mv.Success) {
        $sub = $mv.Groups[1].Value + '.' + $mv.Groups[2].Value
        $subOk = ([double]$sub -le 6.1)
    }

    # Rule 4: forbidden imports. Matching is done on the whole imports dump so it
    # covers both DLL names (Name: combase.dll) and symbol names (Symbol: CoIncrementMTAUsage).
    $win8Hits = @()
    foreach ($k in $forbidden.Keys) {
        if ($imp -match [regex]::Escape($k)) { $win8Hits += $k }
    }

    # AVX-512: only meaningful for binaries we compile. The vendor WebView2 loader
    # is a 5 MB Microsoft DLL; disassembling it is slow and tells us nothing we act on.
    $avx512 = 0
    $skipDisasm = ($name -eq 'WebView2Loader.dll')
    if (-not $skipDisasm) {
        $dis = ((& $od -d --section=.text $f 2>&1) -join "`n")
        $zmm = ([regex]::Matches($dis, 'zmm')).Count
        $mask = ([regex]::Matches($dis, '\{%k')).Count
        $avx512 = $zmm + $mask
    }

    $ok = ($rt.Count -eq 0) -and $subOk -and ($avx512 -eq 0) -and ($win8Hits.Count -eq 0)
    $crtText = if ($rt.Count -eq 0) { 'static' } else { 'dynamic:' + ($rt -join ',') }
    $win7Text = if ($win8Hits.Count -eq 0) { 'ok' } else { ($win8Hits -join ',') }
    $avxText = if ($skipDisasm) { 'n/a' } else { "$avx512" }
    $rows += [pscustomobject]@{
        File   = $name
        Result = $(if ($ok) { 'PASS' } else { 'FAIL' })
        Detail = "crt=$crtText; subsystem=$sub; avx512=$avxText; win7imports=$win7Text"
    }
}

$rows | Format-Table -AutoSize | Out-String | Write-Host
$fails = @($rows | Where-Object { $_.Result -eq 'FAIL' }).Count
Write-Host ("FAIL count: " + $fails)
if ($fails -gt 0) {
    Write-Host ""
    Write-Host "Windows 7 import rules (rule 4):"
    foreach ($k in $forbidden.Keys) { Write-Host ("  {0,-24} {1}" -f $k, $forbidden[$k]) }
}
exit $fails
