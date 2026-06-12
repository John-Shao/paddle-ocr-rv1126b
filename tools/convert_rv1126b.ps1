param(
    [string]$ModelZoo = "D:\workspace\camera\rknn_model_zoo",
    [string]$Python = "python"
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Models = Join-Path $Root "models"
$DetOnnx = Join-Path $Models "ppocrv4_det.onnx"
$RecOnnx = Join-Path $Models "ppocrv4_rec.onnx"
$DetOut = Join-Path $Models "ppocrv4_det_i8.rknn"
$RecOut = Join-Path $Models "ppocrv4_rec_fp16.rknn"

if (!(Test-Path $DetOnnx)) { throw "Missing $DetOnnx" }
if (!(Test-Path $RecOnnx)) { throw "Missing $RecOnnx" }
if (!(Test-Path $ModelZoo)) { throw "Missing Model Zoo directory: $ModelZoo" }

$OldErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& $Python -c "import rknn.api" *> $null
$RknnImportExitCode = $LASTEXITCODE
$ErrorActionPreference = $OldErrorActionPreference
if ($RknnImportExitCode -ne 0) {
    throw "Python cannot import rknn.api. Install RKNN-Toolkit2 in this Python environment first."
}

$DetConvertDir = Join-Path $ModelZoo "examples\PPOCR\PPOCR-Det\python"
$RecConvertDir = Join-Path $ModelZoo "examples\PPOCR\PPOCR-Rec\python"

Write-Host "Converting PPOCR det -> $DetOut"
Push-Location $DetConvertDir
try {
    & $Python "convert.py" $DetOnnx "rv1126b" "i8" $DetOut
    if ($LASTEXITCODE -ne 0) { throw "det conversion failed" }
}
finally {
    Pop-Location
}

Write-Host "Converting PPOCR rec -> $RecOut"
Push-Location $RecConvertDir
try {
    & $Python "convert.py" $RecOnnx "rv1126b" "fp" $RecOut
    if ($LASTEXITCODE -ne 0) { throw "rec conversion failed" }
}
finally {
    Pop-Location
}

Write-Host "Done:"
Get-ChildItem $DetOut, $RecOut | Select-Object FullName, Length
