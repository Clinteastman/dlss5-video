[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Video,

    [Parameter(Mandatory)]
    [string]$Output,

    [string]$Start = '00:00:00',
    [ValidateRange(0.25, 10.0)]
    [double]$Duration = 3.0,
    [ValidateRange(1, 30)]
    [int]$FrameRate = 12,
    [ValidateRange(320, 1920)]
    [int]$Width = 960,
    [string]$RuntimeDirectory
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $RuntimeDirectory) {
    $RuntimeDirectory = Join-Path $repositoryRoot 'private-runtime'
}

$videoPath = (Resolve-Path -LiteralPath $Video).Path
$runtimePath = (Resolve-Path -LiteralPath $RuntimeDirectory).Path.TrimEnd('\')
$probe = Join-Path $runtimePath 'ngx-capability-probe.exe'
if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) {
    throw 'The private harness is missing. Run Prepare-PrivateHarness.ps1 first.'
}
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    throw 'ffmpeg is required to decode and encode the clip.'
}

$outputPath = [System.IO.Path]::GetFullPath($Output)
$outputDirectory = Split-Path -Parent $outputPath
if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
    throw "Output directory does not exist: $outputDirectory"
}

$testId = [guid]::NewGuid().ToString('N')
$tempRoot = Join-Path $runtimePath ".clip-$testId"
$inputFrames = Join-Path $tempRoot 'input'
$outputFrames = Join-Path $tempRoot 'output'
$log = Join-Path $runtimePath 'ReShade.log'
New-Item -ItemType Directory -Path $inputFrames, $outputFrames | Out-Null

try {
    $filter = "fps=$FrameRate,scale=$Width`:-2:flags=lanczos,format=rgb24"
    & ffmpeg -hide_banner -loglevel error -y -ss $Start -i $videoPath -t $Duration `
        -vf $filter -start_number 0 (Join-Path $inputFrames 'frame-%06d.ppm')
    if ($LASTEXITCODE -ne 0) {
        throw 'ffmpeg could not decode the requested clip.'
    }

    $inputCount = @(Get-ChildItem -LiteralPath $inputFrames -Filter '*.ppm').Count
    if ($inputCount -eq 0) {
        throw 'No frames were decoded from the requested time range.'
    }

    Push-Location $runtimePath
    try {
        & $probe $runtimePath --sequence $inputFrames $outputFrames
        if ($LASTEXITCODE -ne 0) {
            throw "The persistent native probe exited with code $LASTEXITCODE."
        }
    }
    finally {
        Pop-Location
    }

    $outputCount = @(Get-ChildItem -LiteralPath $outputFrames -Filter '*.ppm').Count
    if ($outputCount -ne $inputCount) {
        throw "Processed $outputCount of $inputCount frames."
    }
    if (-not (Select-String -LiteralPath $log -SimpleMatch `
        'inline feature 18 evaluation succeeded' -Quiet)) {
        throw "Feature 18 success was not found in $log"
    }

    & ffmpeg -hide_banner -loglevel error -y `
        -framerate $FrameRate -start_number 0 -i (Join-Path $outputFrames 'frame-%06d.ppm') `
        -ss $Start -t $Duration -i $videoPath `
        -map '0:v:0' -map '1:a:0?' -c:v libx264 -preset medium -crf 18 `
        -pix_fmt yuv420p -c:a aac -b:a 192k -shortest -movflags '+faststart' $outputPath
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $outputPath)) {
        throw 'ffmpeg could not encode the processed clip.'
    }

    Write-Host "PASS: $outputCount frames processed in one Neural Rendering session."
    Write-Host "Saved to $outputPath"
}
finally {
    $resolvedTemp = [System.IO.Path]::GetFullPath($tempRoot)
    $runtimePrefix = $runtimePath + [System.IO.Path]::DirectorySeparatorChar
    if ($resolvedTemp.StartsWith($runtimePrefix, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedTemp -PathType Container)) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
