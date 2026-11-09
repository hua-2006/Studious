# Studious 打包脚本：编译 Release → 收集依赖 → 产出可分发目录与 zip
#
# 用法（在任意目录均可）:
#   .\package.ps1                 # 编译 + 打包
#   .\package.ps1 -SkipBuild      # 用现有 frontend\build 直接打包
#   .\package.ps1 -QtRoot D:\Qt -QtVer 6.12.0
#
# 产物:
#   dist\Studious-<版本>-win64\      可直接运行的目录
#   dist\Studious-<版本>-win64.zip   分发包
#
# 不打包的东西:
#   data\assets（模型 / runtime）—— 程序首次运行自行下载，或「资产」页导入
#   data\logs、data\cache —— 运行期生成

param(
    [switch]$SkipBuild,
    [string]$QtRoot = "C:\Code\language\Qt",
    [string]$QtVer  = "6.12.0",
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

Set-Location $PSScriptRoot
$root = $PSScriptRoot

$qtKit   = Join-Path $QtRoot ($QtVer + "\mingw_64")
$qtBin   = Join-Path $qtKit "bin"
$mingw   = Join-Path $QtRoot "Tools\mingw1310_64\bin"
$windeploy = Join-Path $qtBin "windeployqt.exe"
$objdump   = Join-Path $mingw "objdump.exe"

foreach ($p in @($qtBin, $mingw, $windeploy)) {
    if (-not (Test-Path $p)) {
        Write-Host "找不到 $p（用 -QtRoot / -QtVer 指定）" -ForegroundColor Red
        exit 1
    }
}

# ---------- 版本 ----------
$verJson = Get-Content (Join-Path $root "version.json") -Raw | ConvertFrom-Json
$version = $verJson.version
$dist    = Join-Path $root "dist"
$stage   = Join-Path $dist ("Studious-" + $version + "-win64")
$zip     = $stage + ".zip"
Write-Host "版本: $version ($($verJson.codename))  配置: $Config" -ForegroundColor DarkGray

# ---------- 1. 编译 ----------
if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "=== 编译 Release ===" -ForegroundColor Cyan
    & (Join-Path $root "build.ps1") -Config $Config
    if ($LASTEXITCODE -ne 0) { Write-Host "编译失败（exit $LASTEXITCODE）" -ForegroundColor Red; exit 1 }
}

$exe = Join-Path $root "frontend\build\studious.exe"
if (-not (Test-Path $exe)) { Write-Host "找不到 $exe，先编译" -ForegroundColor Red; exit 1 }

# ---------- 2. 清空并铺底 ----------
Write-Host ""
Write-Host "=== 收集文件 ===" -ForegroundColor Cyan
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
if (Test-Path $zip)   { Remove-Item $zip -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item $exe $stage

# 运行期必需：ONNX Runtime（OCR / 本地 MT）
$ortDll = Join-Path $root "backend\third_party\onnxruntime\lib\onnxruntime.dll"
if (Test-Path $ortDll) {
    Copy-Item $ortDll $stage
} else {
    Write-Host "!! 缺少 onnxruntime.dll，OCR 与本地 MT 会起不来" -ForegroundColor Yellow
}

# 版本号是运行期从 version.json 读的，必须随包
Copy-Item (Join-Path $root "version.json") $stage

# 翻译（lrelease 产物在 frontend\build 根）
Get-ChildItem (Join-Path $root "frontend\build") -Filter "studious_*.qm" -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item $_.FullName $stage }

Write-Host "  已拷入 studious.exe / onnxruntime.dll / version.json / *.qm"

# ---------- 3. 部署 Qt 依赖 ----------
Write-Host ""
Write-Host "=== windeployqt ===" -ForegroundColor Cyan
$env:Path = $mingw + ";" + $qtBin + ";" + $env:Path
# 原生工具往 stderr 写警告时，Stop 偏好会把它当异常抛出 —— 这里临时放宽
$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
# --no-translations：Qt 自带的 qt_*.qm（约 7.6 MB）用不上，界面翻译走 qrc 里的 studious_*.qm
# --skip-plugin-types qmltooling：QML 调试插件，发布包里不需要
& $windeploy --release --qmldir (Join-Path $root "frontend\qml") --compiler-runtime --no-system-d3d-compiler `
    --no-translations --skip-plugin-types qmltooling `
    (Join-Path $stage "studious.exe") 2>&1 | ForEach-Object { "  $_" }
$deployCode = $LASTEXITCODE
$ErrorActionPreference = $prevEap
if ($deployCode -ne 0) { Write-Host "windeployqt 失败（exit $deployCode）" -ForegroundColor Red; exit 1 }

# ---------- 4. 依赖自检 ----------
Write-Host ""
Write-Host "=== 依赖自检 ===" -ForegroundColor Cyan
$deps = & $objdump -p (Join-Path $stage "studious.exe") |
    Select-String "DLL Name:" |
    ForEach-Object { ($_ -split "DLL Name:")[1].Trim() }
$missing = @()
foreach ($d in $deps) {
    if ($d -match '^(api-ms-|ext-ms-|KERNEL|USER|GDI|SHELL|ADVAPI|ole32|OLEAUT|WS2_32|COMCTL|WINMM|IMM32|UxTheme|dwmapi|d3d|D3D|dxgi|DXGI|shcore|SHCORE|bcrypt|CRYPT|USERENV|USER32|comdlg32|version|PSAPI|POWRPROF|Secur32|netapi32|dbghelp|NLAAPI|Winhttp|WINHTTP|ntdll|mpr|MPR|WLDAP|IPHLPAPI|setupapi|SETUPAPI|AVRT|mf|MF|propsys|PROPSYS|pdh|PDH|WINSPOOL|winspool|authz|AUTHZ|drprov|ntlanman|davclnt|DAVCLNT)') { continue }
    if (-not (Test-Path (Join-Path $stage $d))) { $missing += $d }
}
if ($missing.Count -gt 0) {
    Write-Host "  以下依赖不在包里（可能由系统提供，也可能真缺）:" -ForegroundColor Yellow
    $missing | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
} else {
    Write-Host "  全部依赖已就地解析" -ForegroundColor Green
}

# ---------- 5. 冒烟测试（用包内 Qt，不借开发环境）----------
Write-Host ""
Write-Host "=== 冒烟测试 ===" -ForegroundColor Cyan
$probe = Join-Path $env:TEMP ("studious-pack-probe-" + $version + ".png")
Remove-Item $probe -ErrorAction SilentlyContinue
$oldPath = $env:Path
$env:Path = $stage + ";" + $oldPath
$ErrorActionPreference = "Continue"
# exe 是 GUI 子系统：用 & 调用时 PowerShell 不会等它结束（那是控制台程序的行为），
# 直接读截图会读到"还没生成"。必须 Start-Process -Wait。
$smokeOut = Join-Path $env:TEMP ("studious-pack-probe-" + $version + ".out")
$smokeErr = Join-Path $env:TEMP ("studious-pack-probe-" + $version + ".err")
$smokeProc = Start-Process -FilePath (Join-Path $stage "studious.exe") `
    -ArgumentList '--ui-shot', $probe, '--ui-size', '1000x700' `
    -PassThru -Wait -RedirectStandardOutput $smokeOut -RedirectStandardError $smokeErr
Get-Content $smokeOut, $smokeErr -ErrorAction SilentlyContinue |
    Where-Object { $_ -match '\S' } | Select-Object -Last 1 | ForEach-Object { "  $_" }
Remove-Item $smokeOut, $smokeErr -ErrorAction SilentlyContinue
$ErrorActionPreference = "Stop"
$env:Path = $oldPath
if (Test-Path $probe) {
    $probeSize = (Get-Item $probe).Length
    Write-Host "  包内程序可独立启动（截图 $probeSize 字节）" -ForegroundColor Green
    Remove-Item $probe -ErrorAction SilentlyContinue
} else {
    Write-Host "  包内程序没能启动，检查上面的依赖清单" -ForegroundColor Red
}

# 冒烟测试是拿包内 exe 真跑了一次，会在包里留下 data\（日志、三个记录库、cache.db）。
# 这些是运行期数据，用户首次启动自己会建 —— 压缩前清掉，别把测试痕迹发出去。
$staleData = Join-Path $stage "data"
if (Test-Path $staleData) {
    Remove-Item $staleData -Recurse -Force
    Write-Host "  已清掉冒烟测试写出的 data\" -ForegroundColor DarkGray
}

# ---------- 6. 打包 ----------
Write-Host ""
Write-Host "=== 压缩 ===" -ForegroundColor Cyan
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal
$sizeMb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host ""
Write-Host "完成:" -ForegroundColor Green
Write-Host "  目录 $( $stage )"
Write-Host "  压缩包 $zip（$sizeMb MB）"
