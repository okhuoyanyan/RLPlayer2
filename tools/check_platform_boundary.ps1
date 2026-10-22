<#
.SYNOPSIS
  平台边界门禁：内核目录里不许出现 Windows 专属头文件 / 类型 / API。

.DESCRIPTION
  扫描 src/dsp、src/engine、src/net、src/meta（含子目录）里的 C/C++ 源文件，
  命中下列任一规则即判失败（退出码 1），并打印 文件:行号。

  规则（只认"代码"内容：注释与字符串字面量先被剥掉 —— 错误文案里出现
        GetLastError= 之类不算越界，那是给用户看的字）：
    1. Windows SDK 头文件：windows.h / winsock2.h / winhttp.h / mmdeviceapi.h / ...
    2. Win32 类型与 API 名：HANDLE / HRESULT / DWORD / HMODULE / MAX_PATH / LoadLibrary / ...
    3. WASAPI / ASIO 专有类型名：IAudioClient / IMMDevice / AUDCLNT_* / WAVEFORMATEX / ...
    4. #pragma comment(lib, ...) 与 __declspec / __stdcall

  平台差异必须收在 src/platform/ 里（见 PLATFORM.md §2 分层图）。

  白名单：tools/platform_boundary_allowlist.txt，每行
      路径|原因|清理期限(yyyy-MM-dd)|责任人
  白名单里的文件照样打印（[ALLOWLISTED]），但不算失败；
  白名单条目若已不再越界，打印 [STALE] 提醒清理（不算失败）。

.PARAMETER Root
  仓库根目录（默认 = 本脚本所在目录的上一级）。
  给 CI 做"门禁自检"时可指向一个只含违规样例的临时目录（铁律 3：门禁必须能变红）。

.EXAMPLE
  powershell -File tools/check_platform_boundary.ps1

.EXAMPLE
  pwsh -File tools/check_platform_boundary.ps1 -Root C:\tmp\probe
#>
[CmdletBinding()]
param(
    [string]$Root = ''
)

$ErrorActionPreference = 'Stop'

# ---------------- 规则 ----------------
$rules = @(
    @{ Name = 'Windows SDK 头文件'
       Re   = '#\s*include\s*[<"](windows|Windows|winsock2|ws2tcpip|winhttp|mmdeviceapi|audioclient|endpointvolume|functiondiscovery|combaseapi|objbase|shellapi|shlobj|psapi|dbghelp|mmsystem|avrt|winmm|winreg|setupapi|knownfolders|propidl|propvarutil|conio)\.h[>"]' },
    @{ Name = 'Win32 类型/API'
       Re   = '\b(HANDLE|HRESULT|DWORD|WORD|BYTE|BOOL|UINT|USHORT|ULONG|HWND|HINSTANCE|HMODULE|HINTERNET|HKEY|HDC|HBRUSH|HFONT|HMENU|HICON|LPWSTR|LPCWSTR|LPSTR|LPCSTR|LPVOID|WPARAM|LPARAM|LRESULT|GUID|CLSID|IUnknown|MAX_PATH|WINAPI|CALLBACK|__declspec|__stdcall|WIN32_LEAN_AND_MEAN|NOMINMAX|LoadLibraryA?|GetProcAddress|FreeLibrary|GetModuleFileNameW?|MultiByteToWideChar|WideCharToMultiByte|MoveFileExW?|CopyFileW|DeleteFileW?|GetLastError|GetTempPathW?|SearchPathW?|CreateProcessW?|CreatePipe|SetHandleInformation|WaitForSingleObject|GetExitCodeProcess|SetConsoleOutputCP|GetFileAttributesW?|CreateDirectoryW?|QueryPerformanceCounter|timeBeginPeriod|VirtualAlloc|GetTickCount64?|CloseHandle|CreateEventW?|SetEvent)\b' },
    @{ Name = 'WASAPI/ASIO 类型'
       Re   = '\b(IMMDevice|IMMDeviceEnumerator|IMMDeviceCollection|IMMNotificationClient|IAudioClient|IAudioRenderClient|IAudioCaptureClient|IAudioClock|ISimpleAudioVolume|IAudioSessionControl|WAVEFORMATEX|WAVEFORMATEXTENSIBLE|AUDCLNT_[A-Z_]+|CLSCTX_[A-Z_]+|CoInitializeEx?|CoCreateInstance|CoTaskMemFree|IASIO|ASIO[A-Za-z]+)\b' },
    @{ Name = '链接/调用约定指令'
       Re   = '#\s*pragma\s+comment\s*\(\s*lib' }
)

$scanDirs = @('src/dsp', 'src/engine', 'src/net', 'src/meta')
$scanExts = @('.h', '.hpp', '.hh', '.inl', '.c', '.cc', '.cpp', '.cxx')

# ---------------- 根目录 ----------------
if ([string]::IsNullOrWhiteSpace($Root)) {
    $rootDir = Split-Path -Parent $PSScriptRoot
} else {
    if (-not (Test-Path -LiteralPath $Root)) {
        Write-Host "[平台边界] 错误：-Root 指向的目录不存在：$Root" -ForegroundColor Red
        exit 2
    }
    $rootDir = (Resolve-Path -LiteralPath $Root).Path
}

# ---------------- 白名单 ----------------
$allowPath = Join-Path $rootDir 'tools/platform_boundary_allowlist.txt'
$allowed   = @{}
if (Test-Path -LiteralPath $allowPath) {
    foreach ($line in (Get-Content -LiteralPath $allowPath -Encoding UTF8)) {
        $t = $line.Trim()
        if ($t.Length -eq 0) { continue }
        if ($t.StartsWith('#')) { continue }
        $parts = $t -split '\|'
        if ($parts.Count -lt 3) {
            Write-Host "[平台边界][WARN] 白名单行格式不对（要 路径|原因|期限|责任人）：$t" -ForegroundColor Yellow
            continue
        }
        $key   = $parts[0].Trim() -replace '\\', '/'
        $owner = ''
        if ($parts.Count -ge 4) { $owner = $parts[3].Trim() }
        $allowed[$key] = @{
            reason   = $parts[1].Trim()
            deadline = $parts[2].Trim()
            owner    = $owner
            hit      = $false
        }
    }
}

# ---------------- 把源文件剥成"只有代码"的行（保持行号） ----------------
function Get-CodeLines {
    param([string]$Text)
    # 块注释 -> 等量换行，行号保持不变
    $noBlock = [regex]::Replace($Text, '(?s)/\*.*?\*/', {
        param($m)
        $n = ([regex]::Matches($m.Value, [string][char]10)).Count
        return [string]::new([char]10, $n)
    })
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($ln in ($noBlock -split "\r?\n")) {
        $code = [regex]::Replace($ln, '"(?:[^"\\]|\\.)*"', '""')     # 字符串字面量
        $code = [regex]::Replace($code, "'(?:[^'\\]|\\.)*'", "''")   # 字符字面量
        $i = $code.IndexOf('//')
        if ($i -ge 0) { $code = $code.Substring(0, $i) }             # 行注释
        $out.Add($code)
    }
    return $out
}

# ---------------- 扫描 ----------------
$files = @()
foreach ($d in $scanDirs) {
    $full = Join-Path $rootDir $d
    if (-not (Test-Path -LiteralPath $full)) { continue }
    $list = @(Get-ChildItem -LiteralPath $full -Recurse -File -ErrorAction SilentlyContinue |
              Where-Object { $scanExts -contains $_.Extension.ToLower() })
    foreach ($it in $list) { $files += [pscustomobject]@{ File = $it; Dir = $d } }
}

$violations = New-Object System.Collections.Generic.List[object]
$exempted   = New-Object System.Collections.Generic.List[object]

foreach ($entry in $files) {
    $f = $entry.File
    # 相对路径：从 FullName 里截出 "<scanDir>/..." 之后的部分。
    # 不用 Substring($rootDir.Length)：$rootDir 可能是 8.3 短名（%TEMP% 常见）而
    # FullName 是规范长名，长度不一致会把路径截歪（本机 CI 自检模拟时踩到过）。
    $norm   = $f.FullName -replace '\\', '/'
    $marker = '/' + $entry.Dir + '/'
    $idx    = $norm.LastIndexOf($marker)
    if ($idx -ge 0) { $rel = $norm.Substring($idx + 1) } else { $rel = $f.Name }
    $text = Get-Content -LiteralPath $f.FullName -Raw -Encoding UTF8
    if ($null -eq $text) { continue }
    $codeLines = Get-CodeLines -Text $text
    $rawLines  = $text -split "\r?\n"
    for ($i = 0; $i -lt $codeLines.Count; $i++) {
        $code = $codeLines[$i]
        if ($code.Trim().Length -eq 0) { continue }
        foreach ($r in $rules) {
            if ([regex]::IsMatch($code, $r.Re)) {
                $raw = $code
                if ($i -lt $rawLines.Count) { $raw = $rawLines[$i] }
                $hit = [pscustomobject]@{
                    Path = $rel
                    Line = $i + 1
                    Text = $raw.Trim()
                    Rule = $r.Name
                }
                if ($allowed.ContainsKey($rel)) {
                    $allowed[$rel].hit = $true
                    $exempted.Add($hit)
                } else {
                    $violations.Add($hit)
                }
                break   # 一行只报一次，避免同一行刷 3 条
            }
        }
    }
}

# ---------------- 报告 ----------------
Write-Host "[平台边界] 仓库根：$rootDir"
Write-Host ("[平台边界] 扫描：{0}（{1} 个源文件）；白名单 {2} 条" -f ($scanDirs -join ', '), $files.Count, $allowed.Count)
Write-Host ''

foreach ($e in $exempted) {
    $a = $allowed[$e.Path]
    Write-Host ("[ALLOWLISTED] {0}:{1}: {2}" -f $e.Path, $e.Line, $e.Text) -ForegroundColor DarkYellow
    Write-Host ("              规则={0}；原因={1}；清理期限={2}；责任人={3}" -f $e.Rule, $a.reason, $a.deadline, $a.owner) -ForegroundColor DarkYellow
}
if ($exempted.Count -gt 0) { Write-Host '' }

foreach ($k in $allowed.Keys) {
    if (-not $allowed[$k].hit) {
        Write-Host ("[STALE] 白名单里的 {0} 已不再越界（原因：{1}），请清理该条目（期限 {2}）" -f $k, $allowed[$k].reason, $allowed[$k].deadline) -ForegroundColor Yellow
    }
}
if ($allowed.Count -gt 0) { Write-Host '' }

if ($violations.Count -eq 0) {
    Write-Host ("[PASS] 内核目录无平台越界（{0} 处已按白名单豁免）" -f $exempted.Count) -ForegroundColor Green
    exit 0
}

foreach ($v in $violations) {
    Write-Host ("[VIOLATION] {0}:{1}: {2}" -f $v.Path, $v.Line, $v.Text) -ForegroundColor Red
    Write-Host ("            规则：{0}" -f $v.Rule) -ForegroundColor Red
}
Write-Host ''
Write-Host ("[FAIL] 发现 {0} 处平台越界（另有 {1} 处白名单豁免）。" -f $violations.Count, $exempted.Count) -ForegroundColor Red
Write-Host "       修法：把平台相关代码搬到 src/platform/（Windows 实现放 src/platform/windows/），" -ForegroundColor Red
Write-Host "             内核只 include platform/*.h。详见 PLATFORM.md §2。" -ForegroundColor Red
exit 1
