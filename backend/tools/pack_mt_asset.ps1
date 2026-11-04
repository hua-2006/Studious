# pack_mt_asset.ps1 —— 把一个本地 MT 语言对目录打包成可分发的 zip，并输出清单条目
#
# 用法：
#   .\backend\tools\pack_mt_asset.ps1 -Pair en-zh
#   .\backend\tools\pack_mt_asset.ps1 -Pair en-zh -ModelsRoot D:\models -OutDir D:\dist
#
# 产物：
#   <OutDir>\mt.opus.<pair>.zip      ← 可直接上传到对象存储 / GitHub Releases
#   控制台打印一段可直接粘进 asset/manifest.json 的 JSON
#
# 为什么需要它：AssetRegistry 只认"单一可下载产物 + sha256"，
# 而一个语言对在本地是 7~8 个文件，必须先压成一个 zip。

param(
    [Parameter(Mandatory = $true)][string]$Pair,
    [string]$ModelsRoot = (Join-Path $env:APPDATA 'studious\assets\models'),
    [string]$OutDir     = (Join-Path $env:APPDATA 'studious\assets\dist')
)

$ErrorActionPreference = 'Stop'

# LocalMtTranslator 要求的最小文件集合（少一个就判定该对不可用）
$required = @(
    'encoder_model_quantized.onnx',
    'decoder_model_quantized.onnx',
    'source.spm',
    'target.spm',
    'vocab.json'
)
# 可选但建议带上（KV cache / 编译配置）
$optional = @(
    'decoder_model_merged_quantized.onnx',
    'config.json',
    'generation_config.json'
)

$srcDir = Join-Path $ModelsRoot ("mt.opus." + $Pair)
if (-not (Test-Path $srcDir)) {
    throw "找不到语言对目录：$srcDir"
}

Write-Host "=== 检查必需文件 ==="
$missing = @()
foreach ($f in $required) {
    $p = Join-Path $srcDir $f
    if (-not (Test-Path $p)) { $missing += $f }
}
if ($missing.Count -gt 0) {
    Write-Host ("缺少： " + ($missing -join ', ')) -ForegroundColor Red
    throw "该语言对不完整，拒绝打包"
}
Write-Host ("  全部就绪（必需 {0} 个）" -f $required.Count) -ForegroundColor Green

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zipPath = Join-Path $OutDir ("mt.opus." + $Pair + ".zip")
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }

Write-Host "`n=== 打包 ==="
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($zipPath, 'Create')
try {
    foreach ($f in ($required + $optional)) {
        $p = Join-Path $srcDir $f
        if (Test-Path $p) {
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $p, $f, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
            Write-Host ("  + " + $f)
        }
    }
} finally {
    $zip.Dispose()
}

$size = (Get-Item $zipPath).Length
$sha  = (Get-FileHash $zipPath -Algorithm SHA256).Hash.ToLower()

Write-Host ("`n产物: {0}" -f $zipPath)
Write-Host ("体积: {0:N0} 字节 ({1:N1} MB)" -f $size, ($size / 1MB))
Write-Host ("sha256: {0}" -f $sha)

# 输出可直接粘贴的清单条目
$filesJson = ($required | ForEach-Object { '          "' + $_ + '"' }) -join ",`n"
$snippet = @"
      {
        "id": "mt.opus.$Pair",
        "type": "model",
        "family": "opus-mt",
        "size": $size,
        "sha256": "$sha",
        "archive": "zip",
        "urls": [],
        "files": [
$filesJson
        ],
        "requiresRamGB": 2,
        "note": "本地 MT 语言对（OPUS-MT int8）。体积约 $([math]::Round($size/1MB)) MB，单句约 100~300 ms，不需要显卡。urls 为空 = 尚未上传，可用「资产 → 导入」手动安装。"
      },
"@
Write-Host "`n=== manifest 条目（urls 待上传后填写）==="
Write-Host $snippet
