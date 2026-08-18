[CmdletBinding()]
param(
    [string]$TizenCli = 'C:\tizen-studio\tools\ide\bin\tizen.bat',
    [string]$SigningProfile
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$projectDirectory = Join-Path $repositoryRoot 'tizen'
$outputDirectory = Join-Path $repositoryRoot 'dist\tizen'
$sourceDirectory = Join-Path $outputDirectory '.source'
$buildDirectory = Join-Path $outputDirectory '.build'
$packageDirectory = Join-Path $outputDirectory '.package'
$artifact = Join-Path $outputDirectory 'MoonlightWebRTC.wgt'

if (-not (Test-Path -LiteralPath $TizenCli -PathType Leaf)) {
    throw "Tizen CLI was not found: $TizenCli"
}
if (-not (Test-Path -LiteralPath (Join-Path $projectDirectory 'config.xml') -PathType Leaf)) {
    throw "Tizen project config is missing: $projectDirectory"
}

if (Test-Path -LiteralPath $buildDirectory) {
    Remove-Item -LiteralPath $buildDirectory -Recurse -Force
}
if (Test-Path -LiteralPath $sourceDirectory) {
    Remove-Item -LiteralPath $sourceDirectory -Recurse -Force
}
if (Test-Path -LiteralPath $packageDirectory) {
    Remove-Item -LiteralPath $packageDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $outputDirectory, $sourceDirectory, $packageDirectory -Force | Out-Null

try {
    $runtimeFiles = @(
        'app.js',
        'application-artwork.js',
        'config.xml',
        'gamepad-input.js',
        'gamepad-ui-navigation.js',
        'gateway-ipv4.js',
        'gateway-store.js',
        'index.html',
        'preferences.js',
        'tizen_web_project.yaml',
        'ui.css',
        'ui.js'
    )
    foreach ($runtimeFile in $runtimeFiles) {
        Copy-Item -LiteralPath (Join-Path $projectDirectory $runtimeFile) -Destination $sourceDirectory
    }
    Copy-Item -LiteralPath (Join-Path $projectDirectory 'assets') -Destination $sourceDirectory -Recurse

    & $TizenCli build-web --output $buildDirectory -- $sourceDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "Tizen web build failed with exit code $LASTEXITCODE."
    }
    Remove-Item -LiteralPath (Join-Path $buildDirectory 'tizen_web_project.yaml') -Force -ErrorAction SilentlyContinue

    $packageArguments = @('package', '-t', 'wgt', '--output', $packageDirectory)
    if (-not [string]::IsNullOrWhiteSpace($SigningProfile)) {
        $packageArguments += @('-s', $SigningProfile)
    }
    $packageArguments += @('--', $buildDirectory)
    & $TizenCli @packageArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Tizen WGT package signing failed with exit code $LASTEXITCODE."
    }

    $packages = @(Get-ChildItem -LiteralPath $packageDirectory -Filter '*.wgt' -File)
    if ($packages.Count -ne 1) {
        throw "Expected one generated WGT, found $($packages.Count)."
    }
    if (Test-Path -LiteralPath $artifact) {
        Remove-Item -LiteralPath $artifact -Force
    }
    Move-Item -LiteralPath $packages[0].FullName -Destination $artifact
    Write-Host "Tizen WGT created: $artifact"
} finally {
    Remove-Item -LiteralPath $sourceDirectory -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $buildDirectory -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $packageDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
