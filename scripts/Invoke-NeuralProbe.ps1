[CmdletBinding()]
param(
    [string]$RuntimeDirectory
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $RuntimeDirectory) {
    $RuntimeDirectory = Join-Path $repositoryRoot 'private-runtime'
}
$probe = Join-Path $RuntimeDirectory 'ngx-capability-probe.exe'
if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) {
    throw 'The private harness is missing. Run Prepare-PrivateHarness.ps1 first.'
}

$log = Join-Path $RuntimeDirectory 'ReShade.log'
& $probe $RuntimeDirectory --evaluate
if ($LASTEXITCODE -ne 0) {
    throw "The native probe exited with code $LASTEXITCODE."
}
if (-not (Test-Path -LiteralPath $log -PathType Leaf)) {
    throw 'ReShade did not create a log, so its proxy was not loaded.'
}

$success = Select-String -LiteralPath $log -SimpleMatch 'inline feature 18 evaluation succeeded' -Quiet
if (-not $success) {
    Write-Host "Probe completed, but Neural Rendering success was not found in $log"
    exit 2
}

Write-Host 'PASS: DLSS Neural Rendering feature 18 evaluated successfully.'
