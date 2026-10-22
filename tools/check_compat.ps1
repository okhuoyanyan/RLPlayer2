# check_compat.ps1 - distribution compatibility gate (ASCII only on purpose)
#
# Why this exists: RLPlayer 1.0.0 was built with -march=znver5 (AVX-512) and
# dynamically linked the MSVC runtime. On a Ryzen 5900X (no AVX-512) the exe died
# with 0xC000001D, and on a clean Windows it would also ask for VCRUNTIME140.dll.
# This script makes those mistakes impossible to ship:
#   1) no VC++ runtime DLL imports (static CRT only)
#   2) PE subsystem version <= 6.1 (Windows 7 loader accepts it)
#   3) no AVX-512 instructions (zmm registers / EVEX masking)
#
# Usage: powershell -File tools/check_compat.ps1 [-Dir build]
param([string]$Dir = "")

$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
if ($Dir -eq "") { $Dir = Join-Path $root 'build' }

$ro = 'C:\Program Files\AMD\ROCm\7.2\bin\llvm-readobj.exe'
$od = 'C:\Program Files\AMD\ROCm\7.2\bin\llvm-objdump.exe'
foreach ($t in @($ro, $od)) {
    if (-not (Test-Path $t)) { Write-Host "missing LLVM tool: $t"; exit 2 }
}

$files = @('RLPlayer.exe', 'rtaweb.exe', 'rtaplay.exe', 'asio_probe.exe', 'rtagpu.dll')
$badRuntime = 'VCRUNTIME|MSVCP|api-ms-win-crt|ucrtbase'
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

    $dis = ((& $od -d --section=.text $f 2>&1) -join "`n")
    $zmm = ([regex]::Matches($dis, 'zmm')).Count
    $mask = ([regex]::Matches($dis, '\{%k')).Count
    $avx512 = $zmm + $mask

    $ok = ($rt.Count -eq 0) -and $subOk -and ($avx512 -eq 0)
    $crtText = if ($rt.Count -eq 0) { 'static' } else { 'dynamic:' + ($rt -join ',') }
    $rows += [pscustomobject]@{
        File   = $name
        Result = $(if ($ok) { 'PASS' } else { 'FAIL' })
        Detail = "crt=$crtText; subsystem=$sub; avx512=$avx512"
    }
}

$rows | Format-Table -AutoSize | Out-String | Write-Host
$fails = @($rows | Where-Object { $_.Result -eq 'FAIL' }).Count
Write-Host ("FAIL count: " + $fails)
exit $fails
