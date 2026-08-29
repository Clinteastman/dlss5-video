[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$StreamlineZip,

    [Parameter(Mandatory)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ReShadeDll,

    [Parameter(Mandatory)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$NeuralAddon,

    [string]$Destination
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $Destination) {
    $Destination = Join-Path $repositoryRoot 'private-runtime'
}
$probe = Join-Path $repositoryRoot 'build\Release\ngx-capability-probe.exe'
if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) {
    throw 'Build the Release configuration before preparing the private harness.'
}
$temporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("dlss5-video-" + [guid]::NewGuid())
try {
    New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
    Expand-Archive -LiteralPath (Resolve-Path -LiteralPath $StreamlineZip) -DestinationPath $temporaryDirectory
    $runtime = Get-ChildItem -LiteralPath $temporaryDirectory -Filter 'nvngx_dlssnr.dll' -File -Recurse |
        Select-Object -First 1 -ExpandProperty DirectoryName
    if (-not $runtime) {
        throw 'The ZIP does not contain nvngx_dlssnr.dll.'
    }

    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Get-ChildItem -LiteralPath $runtime -Filter '*.dll' -File |
        Copy-Item -Destination $Destination -Force
    Copy-Item -LiteralPath $probe -Destination $Destination -Force
    Copy-Item -LiteralPath $ReShadeDll -Destination (Join-Path $Destination 'dxgi.dll') -Force
    Copy-Item -LiteralPath $NeuralAddon -Destination (Join-Path $Destination 'renodx-dlss5.addon64') -Force

    $manifest = Get-ChildItem -LiteralPath $Destination -File | Sort-Object Name | ForEach-Object {
        [pscustomobject]@{
            name = $_.Name
            size = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        }
    }
    $manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Destination 'runtime-manifest.json')
    Write-Host "Private harness prepared at: $Destination"
}
finally {
    if (Test-Path -LiteralPath $temporaryDirectory) {
        Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force
    }
}
