[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$VideoPath,

    [switch]$StartWithVsr
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$configurationDirectory = Join-Path $repositoryRoot 'config'
$mpvCandidates = @(
    (Get-Command mpv.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1),
    'C:\ProgramData\chocolatey\lib\mpvio.install\tools\mpv.exe'
) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -Unique

if (-not $mpvCandidates) {
    throw 'mpv was not found. Install mpv 0.41 or newer and try again.'
}

$mpv = $mpvCandidates[0]
$arguments = @("--config-dir=$configurationDirectory")

if ($StartWithVsr) {
    $arguments += '--vf=@rtx-vsr:d3d11vpp=format=nv12:scale=2:scaling-mode=nvidia'
}

$arguments += (Resolve-Path -LiteralPath $VideoPath).Path

Write-Host "mpv: $mpv"
Write-Host 'Ctrl+V toggles NVIDIA RTX VSR. Ctrl+B bypasses all video filters.'
& $mpv @arguments
