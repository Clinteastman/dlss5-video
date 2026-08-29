[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [string]$Source,

    [ValidateRange(320, 1920)]
    [int]$InputWidth = 960,

    [ValidateRange(180, 1080)]
    [int]$InputHeight = 540,

    [ValidateRange(640, 7680)]
    [int]$WindowWidth = 1920,

    [ValidateRange(360, 4320)]
    [int]$WindowHeight = 1080,

    [ValidateRange(280, 518)]
    [int]$DepthSize = 392,

    [switch]$Loop
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$privateRuntime = Join-Path $repositoryRoot 'private-runtime'
$playerDirectory = Join-Path $privateRuntime 'player'
$mpv = Join-Path $playerDirectory 'mpv.exe'
$guideService = Join-Path $PSScriptRoot 'Live-Guide-Service.py'
$modelCache = Join-Path $privateRuntime 'model-cache'
$ytDlp = Join-Path $privateRuntime 'tools\yt-dlp.exe'
$pythonCandidates = @(
    (Join-Path $env:USERPROFILE 'Miniconda3\python.exe'),
    (Get-Command python.exe -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Source -First 1)
) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } |
    Select-Object -Unique

if (-not (Test-Path -LiteralPath $mpv -PathType Leaf)) {
    throw 'The private ReShade/mpv test harness has not been prepared.'
}
if (-not $pythonCandidates) {
    throw 'Python with the project CUDA dependencies was not found.'
}
$python = @($pythonCandidates)[0]
$isUrl = $Source -match '^https?://'
if (-not $isUrl -and -not (Test-Path -LiteralPath $Source -PathType Leaf)) {
    throw "Video source was not found: $Source"
}
$playbackSource = if ($isUrl) {
    $Source
} else {
    (Resolve-Path -LiteralPath $Source).Path
}

$env:DLSS5_VIDEO_LIVE_GUIDES = '1'
$env:DLSS5_VIDEO_INPUT_WIDTH = [string]$InputWidth
$env:DLSS5_VIDEO_INPUT_HEIGHT = [string]$InputHeight
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$serviceLog = Join-Path $privateRuntime "live-guides-$stamp.log"
$serviceErrorLog = Join-Path $privateRuntime "live-guides-$stamp-error.log"
$serviceArguments = @(
    "`"$guideService`"",
    '--width', [string]$InputWidth,
    '--height', [string]$InputHeight,
    '--depth-size', [string]$DepthSize,
    '--model-cache', "`"$modelCache`""
)

$service = Start-Process `
    -FilePath $python `
    -ArgumentList $serviceArguments `
    -WorkingDirectory $repositoryRoot `
    -RedirectStandardOutput $serviceLog `
    -RedirectStandardError $serviceErrorLog `
    -WindowStyle Hidden `
    -PassThru

try {
    $ready = $false
    $deadline = (Get-Date).AddSeconds(90)
    while ((Get-Date) -lt $deadline -and -not $service.HasExited) {
        if ((Test-Path -LiteralPath $serviceLog) -and
            (Select-String -LiteralPath $serviceLog -SimpleMatch 'Live guides ready' -Quiet)) {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 250
        $service.Refresh()
    }
    if (-not $ready) {
        $detail = if (Test-Path -LiteralPath $serviceErrorLog) {
            Get-Content -LiteralPath $serviceErrorLog -Raw
        } else {
            'No error log was produced.'
        }
        throw "The live guide service did not become ready. $detail"
    }

    $arguments = @(
        '--no-config',
        '--vo=gpu-next',
        '--gpu-api=d3d11',
        "--geometry=${WindowWidth}x${WindowHeight}"
    )
    if ($isUrl -and (Test-Path -LiteralPath $ytDlp -PathType Leaf)) {
        $arguments += "--script-opts=ytdl_hook-ytdl_path=$ytDlp"
    }
    if ($Loop) {
        $arguments += '--loop-file=inf'
    }
    $arguments += $playbackSource
    Write-Host "Live guide input: ${InputWidth}x${InputHeight}"
    Write-Host "Player output: ${WindowWidth}x${WindowHeight}"
    Write-Host "Guide log: $serviceLog"
    $processArguments = $arguments | ForEach-Object {
        if ($_ -match '[\s"]') {
            '"' + $_.Replace('"', '\"') + '"'
        } else {
            $_
        }
    }
    $player = Start-Process `
        -FilePath $mpv `
        -ArgumentList $processArguments `
        -WorkingDirectory $playerDirectory `
        -PassThru
    $player.WaitForExit()
}
finally {
    if (-not $service.HasExited) {
        Stop-Process -Id $service.Id
    }
}
