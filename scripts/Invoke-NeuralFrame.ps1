[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Video,

    [Parameter(Mandatory)]
    [string]$Output,

    [string]$Timestamp = '00:00:00',
    [string]$RuntimeDirectory
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $RuntimeDirectory) {
    $RuntimeDirectory = Join-Path $repositoryRoot 'private-runtime'
}

$videoPath = (Resolve-Path -LiteralPath $Video).Path
$runtimePath = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$probe = Join-Path $runtimePath 'ngx-capability-probe.exe'
if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) {
    throw 'The private harness is missing. Run Prepare-PrivateHarness.ps1 first.'
}
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    throw 'ffmpeg is required to decode and save the frame.'
}

$outputPath = [System.IO.Path]::GetFullPath($Output)
$outputDirectory = Split-Path -Parent $outputPath
if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
    throw "Output directory does not exist: $outputDirectory"
}

$testId = [guid]::NewGuid().ToString('N')
$inputPpm = Join-Path $runtimePath ".frame-input-$testId.ppm"
$outputPpm = Join-Path $runtimePath ".frame-output-$testId.ppm"
$log = Join-Path $runtimePath 'ReShade.log'

try {
    & ffmpeg -hide_banner -loglevel error -y -ss $Timestamp -i $videoPath `
        -frames:v 1 -vf format=rgb24 $inputPpm
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $inputPpm)) {
        throw 'ffmpeg could not decode the requested frame.'
    }

    Push-Location $runtimePath
    try {
        & $probe $runtimePath --frame $inputPpm $outputPpm
        if ($LASTEXITCODE -ne 0) {
            throw "The native frame probe exited with code $LASTEXITCODE."
        }
    }
    finally {
        Pop-Location
    }

    if (-not (Select-String -LiteralPath $log -SimpleMatch `
        'inline feature 18 evaluation succeeded' -Quiet)) {
        throw "The frame completed, but feature 18 success was not found in $log"
    }

    & ffmpeg -hide_banner -loglevel error -y -i $outputPpm $outputPath
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $outputPath)) {
        throw 'ffmpeg could not save the processed image.'
    }

    Write-Host "PASS: Neural Rendering frame saved to $outputPath"
}
finally {
    Remove-Item -LiteralPath $inputPpm, $outputPpm -Force -ErrorAction SilentlyContinue
}
