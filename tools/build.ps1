param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs
)

$ErrorActionPreference = "Stop"
$idfPath = "D:\DEV\esp-idf\v6.0\esp-idf"
$projectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

if (-not (Test-Path $idfPath)) {
    throw "ESP-IDF not found at $idfPath"
}

Remove-Item Env:IDF_PATH -ErrorAction SilentlyContinue
Remove-Item Env:IDF_PYTHON_ENV_PATH -ErrorAction SilentlyContinue
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue

. (Join-Path $idfPath "export.ps1")

Push-Location $projectRoot
try {
    idf.py build @IdfArgs
}
finally {
    Pop-Location
}
