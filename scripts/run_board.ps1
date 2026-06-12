param(
    [string]$BoardHost = "192.168.10.90",
    [string]$BoardUser = "root",
    [string]$BoardPassword = $env:BOARD_PW,
    [string]$BoardDir = "/data/ppocr-text",
    [Alias("Args")]
    [string]$ProgramArgs = ""
)

$ErrorActionPreference = "Stop"

if (!$BoardPassword) {
    throw "Set BOARD_PW or pass -BoardPassword."
}

$Command = "cd '$BoardDir' && LD_LIBRARY_PATH=./lib:/usr/lib ./ppocr_text $ProgramArgs"
& plink -ssh -batch -pw $BoardPassword "$BoardUser@$BoardHost" $Command
if ($LASTEXITCODE -ne 0) { throw "Board command failed" }
