# Studious 一键构建脚本
# 用法: .\build.ps1            # 构建后端 + 前端
#       .\build.ps1 -BackendOnly
#       .\build.ps1 -Test      # 构建后跑后端离线自测

param(
    [switch]$BackendOnly,
    [switch]$Test,
    [string]$QtRoot = "C:\Code\language\Qt",
    [string]$QtVer  = "6.12.0",
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

$qtKit   = Join-Path $QtRoot "$QtVer\mingw_64"
$mingw   = Join-Path $QtRoot "Tools\mingw1310_64\bin"
$cmakeD  = Join-Path $QtRoot "Tools\CMake_64\bin"
$ninjaD  = Join-Path $QtRoot "Tools\Ninja"

foreach ($p in @($qtKit, $mingw, $cmakeD, $ninjaD)) {
    if (-not (Test-Path $p)) { throw "未找到路径: $p  （用 -QtRoot / -QtVer 指定）" }
}

$env:Path = "$mingw;$cmakeD;$ninjaD;$qtKit;" + $env:Path

$cmake = Join-Path $cmakeD "cmake.exe"
$gxx   = Join-Path $mingw "g++.exe"
$gcc   = Join-Path $mingw "gcc.exe"

# Qt 的 CMake 配置需要正斜杠
$prefix = $qtKit.Replace('\', '/')

function Build-Project([string]$name) {
    $src = Join-Path $root $name
    $bld = Join-Path $src "build"
    if (-not (Test-Path (Join-Path $src "CMakeLists.txt"))) {
        Write-Host "[跳过] $name  (无 CMakeLists.txt)" -ForegroundColor DarkYellow
        return $false
    }
    Write-Host "`n=== 配置 $name ===" -ForegroundColor Cyan
    & $cmake -S $src -B $bld -G Ninja "-DCMAKE_BUILD_TYPE=$Config" `
        "-DCMAKE_PREFIX_PATH=$prefix" `
        "-DCMAKE_CXX_COMPILER=$($gxx.Replace('\','/'))" `
        "-DCMAKE_C_COMPILER=$($gcc.Replace('\','/'))"
    if ($LASTEXITCODE -ne 0) { throw "$name 配置失败" }

    Write-Host "`n=== 构建 $name ===" -ForegroundColor Cyan
    & $cmake --build $bld
    if ($LASTEXITCODE -ne 0) { throw "$name 构建失败" }
    return $true
}

Build-Project "backend" | Out-Null

if (-not $BackendOnly) {
    Build-Project "frontend" | Out-Null
}

if ($Test) {
    $st = Join-Path $root "backend\build\backend_selftest.exe"
    if (Test-Path $st) {
        Write-Host "`n=== 后端离线自测 ===" -ForegroundColor Cyan
        & $st --offline
    }
}

Write-Host "`n构建完成。" -ForegroundColor Green
