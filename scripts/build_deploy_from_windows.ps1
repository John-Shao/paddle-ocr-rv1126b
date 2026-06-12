param(
    [string]$UbuntuHost = "192.168.126.129",
    [string]$UbuntuUser = "alientek",
    [string]$UbuntuPassword = $env:UBUNTU_PW,
    [string]$BoardPassword = $env:BOARD_PW,
    [string]$RemoteDir = "/home/alientek/paddle-ocr-rv1126b",
    [switch]$NoDeploy
)

$ErrorActionPreference = "Stop"

if (!$UbuntuPassword) { throw "Set UBUNTU_PW or pass -UbuntuPassword." }
if (!$NoDeploy -and !$BoardPassword) { throw "Set BOARD_PW or pass -BoardPassword, or use -NoDeploy." }

$Sync = Join-Path $PSScriptRoot "sync_to_ubuntu.ps1"
& powershell -NoProfile -ExecutionPolicy Bypass -File $Sync `
    -UbuntuHost $UbuntuHost `
    -UbuntuUser $UbuntuUser `
    -UbuntuPassword $UbuntuPassword `
    -RemoteDir $RemoteDir
if ($LASTEXITCODE -ne 0) { throw "Sync failed" }

$DeployFlag = if ($NoDeploy) { "SKIP_DEPLOY=1" } else { "BOARD_PW='$BoardPassword'" }
$Command = "cd '$RemoteDir' && $DeployFlag scripts/build_deploy_ubuntu.sh"

& plink -ssh -batch -pw $UbuntuPassword "$UbuntuUser@$UbuntuHost" $Command
if ($LASTEXITCODE -ne 0) { throw "Remote build/deploy failed" }

