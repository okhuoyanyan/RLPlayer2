# verify_batch_import.ps1 - 批量入库真实目录结构判据验收脚本
param(
    [int]$Port = 8911,
    [string]$LibDir = "D:\RTA-hqplayer\build-verify2\lib",
    [string]$DataDir = "D:\RTA-hqplayer\build-verify2\testlib_data",
    [string]$RtawebExe = "D:\RTA-hqplayer\build\rtaweb.exe"
)

$ErrorActionPreference = "Stop"

if (!(Test-Path $LibDir)) {
    throw "样本库不存在: $LibDir"
}
if (!(Test-Path $RtawebExe)) {
    throw "rtaweb.exe 不存在: $RtawebExe"
}

# 1. 准备隔离数据目录
if (Test-Path $DataDir) { Remove-Item -Recurse -Force $DataDir }
New-Item -ItemType Directory -Path $DataDir | Out-Null

# 2. 纪律：backend=null, volume=-60
@"
backend=null
volume=-60
"@ | Set-Content -Path (Join-Path $DataDir "rlplayer-settings.conf") -Encoding utf8

# 3. 实例私有库配置
Set-Content -Path (Join-Path $DataDir "rlplayer-library.conf.p$Port") -Value $LibDir -Encoding utf8

$env:RLPLAYER_LIBRARY_DIR = $DataDir
$env:RLPLAYER_PORT = "$Port"

Write-Host "启动 rtaweb: $RtawebExe --port $Port"
$proc = Start-Process -FilePath $RtawebExe -ArgumentList "--port $Port" -PassThru -WindowStyle Hidden
$pidToKill = $proc.Id
Write-Host "进程 PID: $pidToKill (端口: $Port)"

try {
    # 等待服务就绪
    $ready = $false
    for ($i = 0; $i -lt 50; $i++) {
        Start-Sleep -Milliseconds 100
        try {
            $resp = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/ping" -TimeoutSec 1
            if ($resp -match "RLPlayer") { $ready = $true; break }
        } catch {}
    }
    if (!$ready) { throw "rtaweb 无法在端口 $Port 启动" }

    # 触发真实批量扫描
    $scanResp = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/lib/scan" -Method Post -Body "full=1"
    Write-Host "已触发全量扫描: $(ConvertTo-Json $scanResp -Compress)"

    # 轮询直到扫描完成
    for ($i = 0; $i -lt 100; $i++) {
        Start-Sleep -Milliseconds 100
        $stats = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/lib/stats"
        if (!$stats.scanning) {
            Write-Host "扫描完成: tracks=$($stats.tracks), albums=$($stats.albums), 耗时=$($stats.lastScanMs)ms"
            break
        }
    }

    # 查询专辑清单
    $albumsRes = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/lib/albums"
    Write-Host ""
    Write-Host "=================== 真实入库专辑验收结果 ==================="
    Write-Host "专辑总数: $($albumsRes.albums.Count)"

    $albumMap = @{}
    foreach ($a in $albumsRes.albums) {
        $key = $a.k
        $detail = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/lib/album?k=$([uri]::EscapeDataString($key))"
        $albumMap[$a.name] = @{
            Key = $a.k
            Artist = $a.artist
            TrackCount = $a.n
            Tracks = $detail.tracks
        }
        Write-Host "专辑: [$($a.name)] (曲目数: $($a.n), 键: $($a.k))"
        foreach ($t in $detail.tracks) {
            Write-Host "   -> 序号=$($t.i) 碟号=$($t.d) 轨号=$($t.n) 标题='$($t.title)' 路径='$($t.path)'"
        }
    }

    # 断言
    Write-Host ""
    Write-Host "=================== 断言核验 ==================="
    $pass = $true

    # 断言 1: 专辑总数为 3
    if ($albumsRes.albums.Count -eq 3) {
        Write-Host "[PASS] 专辑总数严格为 3"
    } else {
        Write-Host "[FAIL] 专辑总数预期 3，实际 $($albumsRes.albums.Count)"
        $pass = $false
    }

    # 断言 2: BoxSetAlbum 包含 4 首曲目 (CD1/CD2 合并)
    if ($albumMap.ContainsKey("BoxSetAlbum") -and $albumMap["BoxSetAlbum"].TrackCount -eq 4) {
        Write-Host "[PASS] CD1/CD2 成功合并为单张专辑 BoxSetAlbum (共 4 首)"
        $paths = $albumMap["BoxSetAlbum"].Tracks | ForEach-Object { $_.path }
        $hasCD1 = ($paths | Where-Object { $_ -match "CD1" }).Count -eq 2
        $hasCD2 = ($paths | Where-Object { $_ -match "CD2" }).Count -eq 2
        if ($hasCD1 -and $hasCD2) {
            Write-Host "[PASS] BoxSetAlbum 正确包含 CD1 (2首) 与 CD2 (2首)"
        } else {
            Write-Host "[FAIL] BoxSetAlbum 曲目归属异常: $paths"
            $pass = $false
        }
    } else {
        Write-Host "[FAIL] BoxSetAlbum 未正确收敛"
        $pass = $false
    }

    # 断言 3: 分类目录 Compilations 未被当成专辑，SummerHits 独立为专辑 (2首)
    if (!$albumMap.ContainsKey("Compilations") -and $albumMap.ContainsKey("SummerHits") -and $albumMap["SummerHits"].TrackCount -eq 2) {
        Write-Host "[PASS] 分类目录 Compilations 未被误当专辑，实际专辑 SummerHits 独立成辑 (2首)"
    } else {
        Write-Host "[FAIL] 分类目录判据异常 (Compilations 是否被当成专辑: $($albumMap.ContainsKey('Compilations')))"
        $pass = $false
    }

    # 断言 4: misc 子目录被排除，SoloAlbum 包含 2 首曲目
    if (!$albumMap.ContainsKey("misc") -and $albumMap.ContainsKey("SoloAlbum") -and $albumMap["SoloAlbum"].TrackCount -eq 2) {
        Write-Host "[PASS] misc 子目录被成功排除（不建专辑），SoloAlbum 独立成辑 (2首)"
    } else {
        Write-Host "[FAIL] misc 子目录未被正确排除"
        $pass = $false
    }

    if (!$pass) { throw "断言未全部通过" }
    Write-Host ""
    Write-Host "全部断言 PASS！"
} finally {
    if ($pidToKill) {
        Write-Host "清理进程 PID: $pidToKill"
        Stop-Process -Id $pidToKill -Force
    }
}
