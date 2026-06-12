param(
    [string]$UbuntuHost = "192.168.126.129",
    [string]$UbuntuUser = "alientek",
    [string]$UbuntuPassword = $env:UBUNTU_PW,
    [string]$RemoteDir = "/home/alientek/paddle-ocr-rv1126b"
)

$ErrorActionPreference = "Stop"

if (!$UbuntuPassword) {
    throw "Set UBUNTU_PW or pass -UbuntuPassword."
}

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

function Run-Plink($Command) {
    & plink -ssh -batch -pw $UbuntuPassword "$UbuntuUser@$UbuntuHost" $Command
    if ($LASTEXITCODE -ne 0) { throw "Remote command failed: $Command" }
}

function Copy-One($Source, $TargetDir) {
    & pscp -batch -pw $UbuntuPassword $Source "$UbuntuUser@$UbuntuHost`:$TargetDir/"
    if ($LASTEXITCODE -ne 0) { throw "Copy failed: $Source -> $TargetDir" }
}

Run-Plink "mkdir -p '$RemoteDir/src' '$RemoteDir/scripts' '$RemoteDir/tools' '$RemoteDir/models'"

Copy-One (Join-Path $Root "CMakeLists.txt") $RemoteDir
Copy-One (Join-Path $Root "README_CN.md") $RemoteDir
Copy-One (Join-Path $Root "src\ppocr_text.cc") "$RemoteDir/src"
Copy-One (Join-Path $Root "scripts\build_deploy_ubuntu.sh") "$RemoteDir/scripts"
Copy-One (Join-Path $Root "scripts\sync_to_ubuntu.ps1") "$RemoteDir/scripts"
Copy-One (Join-Path $Root "scripts\build_deploy_from_windows.ps1") "$RemoteDir/scripts"
Copy-One (Join-Path $Root "scripts\run_board.ps1") "$RemoteDir/scripts"
Copy-One (Join-Path $Root "tools\convert_rv1126b.ps1") "$RemoteDir/tools"
Copy-One (Join-Path $Root "tools\prepare_model_zoo_demo.ps1") "$RemoteDir/tools"

$DetOnnx = Join-Path $Root "models\ppocrv4_det.onnx"
$RecOnnx = Join-Path $Root "models\ppocrv4_rec.onnx"
if (Test-Path $DetOnnx) { Copy-One $DetOnnx "$RemoteDir/models" }
if (Test-Path $RecOnnx) { Copy-One $RecOnnx "$RemoteDir/models" }

Run-Plink "chmod +x '$RemoteDir/scripts/build_deploy_ubuntu.sh'"

Write-Host "Synced to $UbuntuUser@$UbuntuHost`:$RemoteDir"
