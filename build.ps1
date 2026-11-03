# Studious 一键构建脚本
#
# 用法（在任意目录均可）:
#   .\build.ps1                 # 构建后端 + 前端
#   .\build.ps1 -BackendOnly    # 只构建后端
#   .\build.ps1 -Test           # 构建后跑后端离线自测
#   .\build.ps1 -QtRoot D:\Qt -QtVer 6.12.0

param(
    [switch]$BackendOnly,
    [switch]$Test,
    [string]$QtRoot = "C:\Code\language\Qt",
    [string]$QtVer  = "6.12.0",
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

# PS 5.1 控制台默认 cp936，先切 UTF-8，否则中文输出乱码
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

Set-Location $PSScriptRoot
$root = $PSScriptRoot

# ---------- 路径检查 ----------
$qtKit  = Join-Path $QtRoot ($QtVer + "\mingw_64")
$qtBin  = Join-Path $qtKit "bin"
$mingw  = Join-Path $QtRoot "Tools\mingw1310_64\bin"
$cmakeD = Join-Path $QtRoot "Tools\CMake_64\bin"
$ninjaD = Join-Path $QtRoot "Tools\Ninja"

$missing = @()
foreach ($p in @($qtKit, $qtBin, $mingw, $cmakeD, $ninjaD)) {
    if (-not (Test-Path $p)) { $missing += $p }
}
if ($missing.Count -gt 0) {
    Write-Host "找不到以下路径:" -ForegroundColor Red
    foreach ($m in $missing) { Write-Host "  $m" -ForegroundColor Red }
    Write-Host "用 -QtRoot / -QtVer 指定 Qt 安装位置，例如: .\build.ps1 -QtRoot D:\Qt -QtVer 6.12.0" -ForegroundColor Yellow
    exit 1
}

# 注意：必须把 mingw_64\bin 加进 PATH，否则运行期找不到 Qt6Core.dll
# （缺 DLL 时进程直接 0xC0000135 结束，且不会有任何输出）
$env:Path = $mingw + ";" + $cmakeD + ";" + $ninjaD + ";" + $qtBin + ";" + $env:Path

$cmake  = Join-Path $cmakeD "cmake.exe"
$gxx    = Join-Path $mingw  "g++.exe"
$prefix = $qtKit.Replace('\', '/')
$gxxFwd = $gxx.Replace('\', '/')

# ---------- 统一的命令执行器 ----------
# 捕获子进程输出再逐行打印：既保证任何情况下都能看到输出，
# 也避免 `函数 | Out-Null` 把 cmake/ninja 的日志一起吞掉。
function Invoke-Tool {
    param(
        [string]   $Exe,
        [string[]] $Argv,
        [string]   $Label
    )
    $lines = & $Exe @Argv 2>&1 | ForEach-Object { "$_" }
    $code  = $LASTEXITCODE
    foreach ($l in $lines) { Write-Host "    $l" }
    if ($code -ne 0) {
        Write-Host "!! $Label 失败（exit $code）" -ForegroundColor Red
        exit $code
    }
}

function Build-Project {
    param([string]$Name)

    $src = Join-Path $root $Name
    if (-not (Test-Path (Join-Path $src "CMakeLists.txt"))) {
        Write-Host "[跳过] $Name（没有 CMakeLists.txt）" -ForegroundColor DarkYellow
        return
    }
    $bld = Join-Path $src "build"

    Write-Host ""
    Write-Host "=== 配置 $Name ===" -ForegroundColor Cyan
    Invoke-Tool $cmake @(
        "-S", $src, "-B", $bld, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=$Config",
        "-DCMAKE_PREFIX_PATH=$prefix",
        "-DCMAKE_CXX_COMPILER=$gxxFwd"
    ) "配置 $Name"

    Write-Host ""
    Write-Host "=== 构建 $Name ===" -ForegroundColor Cyan
    Invoke-Tool $cmake @("--build", $bld) "构建 $Name"
}

# ---------- 主流程 ----------
Write-Host "Qt: $qtKit" -ForegroundColor DarkGray
Write-Host "配置: $Config" -ForegroundColor DarkGray

Build-Project "backend"

if (-not $BackendOnly) {
    Build-Project "frontend"
}

if ($Test) {
    $st = Join-Path $root "backend\build\backend_selftest.exe"
    Write-Host ""
    Write-Host "=== 后端离线自测 ===" -ForegroundColor Cyan
    if (Test-Path $st) {
        Invoke-Tool $st @("--offline") "后端自测"
        Write-Host "    >>> 自测通过" -ForegroundColor Green
    } else {
        Write-Host "    找不到 $st" -ForegroundColor Red
        exit 1
    }
}

Write-Host ""
Write-Host "构建完成。" -ForegroundColor Green
Write-Host "运行 GUI: .\frontend\build\studious.exe" -ForegroundColor Green
