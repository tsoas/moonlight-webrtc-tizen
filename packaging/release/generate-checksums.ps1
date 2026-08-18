[CmdletBinding()]
param(
    [string]$DistributionDirectory
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if ([string]::IsNullOrWhiteSpace($DistributionDirectory)) {
    $DistributionDirectory = Join-Path $repositoryRoot 'dist'
}
$DistributionDirectory = [System.IO.Path]::GetFullPath($DistributionDirectory)

$assets = @(
    [pscustomobject]@{
        Name = 'MoonlightWebRTC-Setup.exe'
        Path = Join-Path $DistributionDirectory 'windows\MoonlightWebRTC-Setup.exe'
    },
    [pscustomobject]@{
        Name = 'MoonlightWebRTC.wgt'
        Path = Join-Path $DistributionDirectory 'tizen\MoonlightWebRTC.wgt'
    },
    [pscustomobject]@{
        Name = 'MoonlightWebRTC-Source.tar.gz'
        Path = Join-Path $DistributionDirectory 'MoonlightWebRTC-Source.tar.gz'
    }
)

$lines = foreach ($asset in $assets) {
    if (-not (Test-Path -LiteralPath $asset.Path -PathType Leaf)) {
        throw "Required release asset is missing: $($asset.Path)"
    }
    $hash = (Get-FileHash -LiteralPath $asset.Path -Algorithm SHA256).Hash.ToLowerInvariant()
    '{0}  {1}' -f $hash, $asset.Name
}

$checksumPath = Join-Path $DistributionDirectory 'SHA256SUMS.txt'
Set-Content -LiteralPath $checksumPath -Value $lines -Encoding ascii
Write-Host "Release checksums created: $checksumPath"
