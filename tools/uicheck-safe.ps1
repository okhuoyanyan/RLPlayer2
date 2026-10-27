param(
  [string]$Url = "http://127.0.0.1:8899/",
  [string]$ExeDir = "",
  [string]$RepoRoot = ""
)

$ErrorActionPreference = "Continue"
if (-not $RepoRoot) { $RepoRoot = Split-Path -Parent $PSScriptRoot }
if (-not $ExeDir) {
  $cand = Get-ChildItem (Join-Path $RepoRoot "dist") -Directory -Filter "RLPlayer-*-win64" -ErrorAction SilentlyContinue |
          Sort-Object Name -Descending | Select-Object -First 1
  if ($cand) { $ExeDir = $cand.FullName }
}
if (-not $ExeDir -or -not (Test-Path $ExeDir)) { Write-Host "[uicheck-safe] exe dir not found, abort" -ForegroundColor Red; exit 2 }

# 2026-10-27：不只队列，**设置文件**也会被自检污染。
# 事故：跑完 uicheck 后 rlplayer-settings.conf 里 metasummary 从 1 变成 0（与 metallm=1 组成混合态），
#       艺术家资料页因此不再渲染 AI 卡，3 条资料页断言变红 —— 看上去像代码回归，其实是自检改了用户配置。
# 现在把下面这些「用户状态文件」**全部**纳入备份/还原。
$stateFiles = @(
  "rlplayer-playlist.txt",
  "rlplayer-settings.conf",
  "rlplayer-library.conf",
  "rlplayer-favs.json",
  "rlplayer-savedqueues.conf",
  "rlplayer-llm.conf",
  "rlplayer-update.conf"
)

$bak = Join-Path $ExeDir ".uicheck-state-bak"
if (Test-Path $bak) { Remove-Item $bak -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $bak | Out-Null

$saved = @()
foreach ($n in $stateFiles) {
  $p = Join-Path $ExeDir $n
  if (Test-Path $p) {
    Copy-Item $p (Join-Path $bak $n) -Force
    $saved += $n
  }
}
Write-Host ("[uicheck-safe] backed up " + $saved.Count + " state file(s): " + ($saved -join ", ")) -ForegroundColor Cyan

function Count-Items([string]$p) { if (-not (Test-Path $p)) { return 0 }; return (Select-String -Path $p -Pattern "^item=" -Encoding UTF8 | Measure-Object).Count }
function Get-Kv([string]$p, [string]$k) {
  if (-not (Test-Path $p)) { return "" }
  $m = Select-String -Path $p -Pattern ("^" + $k + "=(.*)$") -Encoding UTF8 | Select-Object -First 1
  if ($m) { return $m.Matches[0].Groups[1].Value }
  return ""
}

$origQ = Count-Items (Join-Path $ExeDir "rlplayer-playlist.txt")
$origSumm = Get-Kv (Join-Path $ExeDir "rlplayer-settings.conf") "metasummary"
$origLlm  = Get-Kv (Join-Path $ExeDir "rlplayer-settings.conf") "metallm"
Write-Host "[uicheck-safe] before: queue=$origQ items, metallm=$origLlm, metasummary=$origSumm" -ForegroundColor Cyan

$code = 1
try {
  Push-Location $RepoRoot
  node (Join-Path $RepoRoot "tools\uicheck.js") $Url
  $code = $LASTEXITCODE
} finally {
  Pop-Location
  # 先让应用把内存里的设置刷盘，再用备份覆盖（应用没在跑时才直接覆盖）
  foreach ($n in $saved) {
    $src = Join-Path $bak $n
    $dst = Join-Path $ExeDir $n
    if (Test-Path $src) { Copy-Item $src $dst -Force }
  }
  $nowQ = Count-Items (Join-Path $ExeDir "rlplayer-playlist.txt")
  $nowSumm = Get-Kv (Join-Path $ExeDir "rlplayer-settings.conf") "metasummary"
  $nowLlm  = Get-Kv (Join-Path $ExeDir "rlplayer-settings.conf") "metallm"
  if ($nowQ -eq $origQ -and $nowSumm -eq $origSumm -and $nowLlm -eq $origLlm) {
    Write-Host "[uicheck-safe] state restored: queue=$nowQ, metallm=$nowLlm, metasummary=$nowSumm" -ForegroundColor Green
  } else {
    Write-Host "[uicheck-safe] RESTORE MISMATCH: queue $nowQ vs $origQ, metallm $nowLlm vs $origLlm, metasummary $nowSumm vs $origSumm" -ForegroundColor Red
    Write-Host "[uicheck-safe] NOTE: if the app is still running it may overwrite these on exit; restart it." -ForegroundColor Yellow
  }
  Remove-Item $bak -Recurse -Force -ErrorAction SilentlyContinue
}
exit $code