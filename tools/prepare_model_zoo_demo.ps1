param(
    [string]$ModelZoo = "D:\workspace\camera\rknn_model_zoo"
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Models = Join-Path $Root "models"
$DetSrc = Join-Path $Models "ppocrv4_det_i8.rknn"
$RecSrc = Join-Path $Models "ppocrv4_rec_fp16.rknn"

if (!(Test-Path $DetSrc)) { throw "Missing $DetSrc. Run tools\convert_rv1126b.ps1 first." }
if (!(Test-Path $RecSrc)) { throw "Missing $RecSrc. Run tools\convert_rv1126b.ps1 first." }
if (!(Test-Path $ModelZoo)) { throw "Missing Model Zoo directory: $ModelZoo" }

$DetDst = Join-Path $ModelZoo "examples\PPOCR\PPOCR-Det\model\ppocrv4_det.rknn"
$RecDst = Join-Path $ModelZoo "examples\PPOCR\PPOCR-Rec\model\ppocrv4_rec.rknn"

Copy-Item $DetSrc $DetDst -Force
Copy-Item $RecSrc $RecDst -Force

Write-Host "Copied RKNN models into Model Zoo demo:"
Get-ChildItem $DetDst, $RecDst | Select-Object FullName, Length

