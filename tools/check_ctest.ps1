# Strict CTest result gate: discovery, exit status and executed (not skipped) count.
# 2026-11-14（3.3.11 门禁提速）：新增 -Parallel / -SerialTests 两段式。
#   实测单纯 --parallel 4：1602 s → 777 s，但 app_enrich_budget 与 controls336 因 CPU 争抢失败。
#   原因：这两个用例的判据本身就是「墙钟时间」（HTTP 预算 700 ms、UI 首帧时限），
#   并行时机器负载把它们推过阈值 —— 是判据对负载敏感，不是被测代码错了。
#   所以：可并行的批量先并行跑，负载敏感的这批**单独串行**跑，两段结果合并后再做严格计数。
param([Parameter(Mandatory=$true)][string]$BuildDir,[string]$Include='',[string]$Exclude='',[string]$Report='',[int]$Parallel=0,[string]$SerialTests='')
$ErrorActionPreference='Stop'
if (-not (Test-Path -LiteralPath $BuildDir -PathType Container)) { throw "Build directory missing: $BuildDir" }
if (-not $Report) { $Report=Join-Path $BuildDir ('ctest-gate-'+[guid]::NewGuid().ToString('N')+'.xml') }
if (Test-Path -LiteralPath $Report) { throw "Report must be fresh: $Report" }
$base=@('--test-dir',$BuildDir,'--no-tests=error','--output-on-failure','--test-output-size-passed','10485760','--test-output-size-failed','10485760')
if ($Include) {$base+=@('-R',$Include)}
if ($Exclude) {$base+=@('-E',$Exclude)}
$oldDevice=$env:RTA_NO_DEVICE; $oldNet=$env:RTA_NET_TESTS; $oldOpt=$env:PYTHONOPTIMIZE
$reports=@()
try {
  $env:RTA_NO_DEVICE='1'; $env:RTA_NET_TESTS='0'; $env:PYTHONOPTIMIZE=''
  $ErrorActionPreference='Continue'
  $rcAll=0
  # 2026-11-16（用户：错一个为什么要重跑全部）：迭代时可以让门禁**首红即停**。
  #   默认关闭（发布口径不变：跑完全部再汇总）；设 RTA_GATE_FAILFAST=1 时，
  #   第一阶段一红就不再跑第二阶段，几秒钟就能看到是哪一个失败。
  $failFast=($env:RTA_GATE_FAILFAST -eq '1')
  if($failFast){ $base+=@('--stop-on-failure'); Write-Host 'GATE fail-fast enabled (RTA_GATE_FAILFAST=1)' }
  $twoPhase=($Parallel -gt 1) -and (-not [string]::IsNullOrWhiteSpace($SerialTests))
  if ($twoPhase) {
    $par=$Report+'.par.xml'; $ser=$Report+'.ser.xml'
    Write-Host ("GATE phase 1/2: parallel=$Parallel (excluding load-sensitive set)")
    $a1=$base+@('--parallel',[string]$Parallel,'-E',$SerialTests,'--output-junit',$par)
    & ctest @a1
    if ($LASTEXITCODE -ne 0) {$rcAll=1}
    if($failFast -and $rcAll -ne 0){ Write-Host 'GATE fail-fast: phase 1 had failures, skipping phase 2'; $twoPhase=$false }
    if($twoPhase){ Write-Host 'GATE phase 2/2: load-sensitive set, serial'
    # 2026-11-14：第二段必须允许"零匹配" —— 当调用方用 -Include 只选了一小批测试
    # （例如 verify_ffmpeg_lite 只选 ffmpeg 相关），这一批里可能一个负载敏感项都没有，
    # 此时 --no-tests=error 会把"本来就该空"判成失败。空集合的守门交给第一段与末尾的
    # executed<=0 检查（合并后仍然生效）。
    $baseNoErr=@($base | Where-Object { $_ -ne '--no-tests=error' })
    $a2=$baseNoErr+@('-R',$SerialTests,'--output-junit',$ser)
    & ctest @a2
    if ($LASTEXITCODE -ne 0) {$rcAll=1}
    }
    if(Test-Path -LiteralPath $par){ $reports+= $par }
    if(Test-Path -LiteralPath $ser){ $reports+= $ser }
  } else {
    $a=$base
    if ($Parallel -gt 0) {$a=$a+@('--parallel',[string]$Parallel)}
    $a=$a+@('--output-junit',$Report)
    & ctest @a
    $rcAll=$LASTEXITCODE
    $reports=@($Report)
  }
  $ErrorActionPreference='Stop'
  foreach($rp in $reports){ if (-not (Test-Path -LiteralPath $rp -PathType Leaf)) {throw "CTest did not create a result report: $rp"} }
  $cases=@()
  foreach($rp in $reports){ [xml]$x=Get-Content -LiteralPath $rp -Raw; $cases+=@($x.SelectNodes('//testcase')) }
  if ($rcAll -ne 0) {throw "CTest failed: exit=$rcAll"}
  $skipped=@($cases | Where-Object {
    $out=$_.SelectSingleNode('system-out')
    $_.SelectSingleNode('skipped') -or ($out -and $out.InnerText -match '(?i)\bSKIP\b|SKIP_ME|UNVERIFIED_(?:REAL|DEVICE)|UNCOVERED' -and $out.InnerText -notmatch '(?i)\bPASS\b')
  }).Count
  $partial=@($cases | Where-Object {
    $out=$_.SelectSingleNode('system-out')
    $out -and $out.InnerText -match '(?i)\bSKIP\b|SKIP_ME|UNVERIFIED_(?:REAL|DEVICE)|UNCOVERED' -and $out.InnerText -match '(?i)\bPASS\b'
  })
  if($partial.Count -gt 0) {
    Write-Host ('UNVERIFIED branches (explicit SKIP alongside passing assertions): '+(($partial | ForEach-Object {$_.name}) -join ', '))
  }
  $failed=@($cases | Where-Object {$_.SelectSingleNode('failure') -or $_.SelectSingleNode('error')}).Count
  $executed=$cases.Count-$skipped
  if ($executed -le 0 -or $failed -gt 0 -or $skipped -gt 0) {throw "Invalid coverage: total=$($cases.Count), executed=$executed, skipped=$skipped, failed=$failed"}
  Write-Host "CTest gate PASS: total=$($cases.Count), executed=$executed, skipped=$skipped (skips are not coverage), failed=$failed; partial-skip-tests=$($partial.Count)"
} finally {$env:RTA_NO_DEVICE=$oldDevice;$env:RTA_NET_TESTS=$oldNet;$env:PYTHONOPTIMIZE=$oldOpt}