[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ZipPath
)

$ErrorActionPreference = 'Stop'
$resolvedZip = (Resolve-Path -LiteralPath $ZipPath).Path
$temporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("dlss5-video-" + [guid]::NewGuid())

try {
    New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
    Expand-Archive -LiteralPath $resolvedZip -DestinationPath $temporaryDirectory

    Write-Output ([pscustomobject]@{
        Package = $resolvedZip
        SHA256 = (Get-FileHash -LiteralPath $resolvedZip -Algorithm SHA256).Hash
    })

    Get-ChildItem -LiteralPath $temporaryDirectory -Filter '*.dll' -File -Recurse |
        Sort-Object Name |
        ForEach-Object {
            $signature = Get-AuthenticodeSignature -LiteralPath $_.FullName
            [pscustomobject]@{
                Name = $_.Name
                Size = $_.Length
                Version = $_.VersionInfo.FileVersion
                Signature = $signature.Status
                SHA256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
            }
        }
}
finally {
    if (Test-Path -LiteralPath $temporaryDirectory) {
        Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force
    }
}
