param(
    [int]$Port = 8848
)
$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $projectRoot
$binary = Join-Path $projectRoot "build\Release\lab_telemetry.exe"
if (-not (Test-Path -LiteralPath $binary)) {
    throw "未找到 $binary，请先按 README.md 构建。"
}
& $binary $Port
