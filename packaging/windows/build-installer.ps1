[CmdletBinding()]
param(
    [string]$BuildDirectory,
    [string]$OutputDirectory,
    [string]$InnoSetupCompiler
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $PSScriptRoot '..\..\build-vcpkg-release'
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $PSScriptRoot '..\..\dist\windows'
}

function Resolve-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path)
}

function Find-VisualStudioFile {
    param([Parameter(Mandatory = $true)][string]$LeafName)

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $installationPath = (& $vswhere -latest -products * -property installationPath | Select-Object -First 1).Trim()
        if ($installationPath) {
            $candidate = Get-ChildItem -LiteralPath $installationPath -Recurse -Filter $LeafName -File |
                Select-Object -First 1
            if ($candidate) {
                return $candidate.FullName
            }
        }
    }
    throw "Unable to locate $LeafName in the installed Visual Studio toolchain."
}

function Find-InnoSetupCompiler {
    param([string]$RequestedPath)

    $candidates = @($RequestedPath, (Get-Command ISCC.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue),
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
        'C:\Program Files (x86)\Inno Setup 6\ISCC.exe', 'C:\Program Files\Inno Setup 6\ISCC.exe') |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw 'Inno Setup Compiler (ISCC.exe) was not found. Install Inno Setup 6 or pass -InnoSetupCompiler.'
}

function Get-PeDependents {
    param(
        [Parameter(Mandatory = $true)][string]$Dumpbin,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $output = & $Dumpbin /DEPENDENTS $Path
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed for $Path"
    }
    return @($output | ForEach-Object {
        if ($_ -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') {
            $Matches[1]
        }
    } | Where-Object { $_ } | Select-Object -Unique)
}

function Test-SystemOrVcRuntime {
    param([Parameter(Mandatory = $true)][string]$Dependency)

    $name = $Dependency.ToLowerInvariant()
    if ($name -like 'api-ms-win-*.dll' -or $name -like 'ext-ms-win-*.dll') {
        return $true
    }
    if ($name -match '^(msvcp\d+(_atomic_wait)?|vcruntime\d+(_\d+)?|concrt\d+)\.dll$') {
        return $true
    }
    return @('advapi32.dll', 'bcrypt.dll', 'crypt32.dll', 'dwmapi.dll', 'gdi32.dll', 'iphlpapi.dll', 'kernel32.dll',
        'ole32.dll', 'shell32.dll', 'user32.dll', 'winmm.dll', 'ws2_32.dll') -contains $name
}

$repositoryRoot = Resolve-FullPath (Join-Path $PSScriptRoot '..\..')
$buildDirectory = Resolve-FullPath $BuildDirectory
$outputDirectory = Resolve-FullPath $OutputDirectory
$stageDirectory = Join-Path $outputDirectory 'stage'
$issPath = Join-Path $PSScriptRoot 'MoonlightWebRTC.iss'

foreach ($requiredFile in @('moonlight_webrtc.exe', 'moonlight_webrtc_tray.exe')) {
    $path = Join-Path $buildDirectory $requiredFile
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required Release binary is missing: $path"
    }
}

$dumpbin = Find-VisualStudioFile 'dumpbin.exe'
$vcRedist = Find-VisualStudioFile 'vc_redist.x64.exe'
$iscc = Find-InnoSetupCompiler $InnoSetupCompiler

if (Test-Path -LiteralPath $stageDirectory) {
    Remove-Item -LiteralPath $stageDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $stageDirectory -Force | Out-Null

$searchDirectories = @($buildDirectory, (Join-Path $buildDirectory 'vcpkg_installed\x64-windows\bin')) |
    Where-Object { Test-Path -LiteralPath $_ -PathType Container }
$dependencyFiles = @{}
foreach ($directory in $searchDirectories) {
    Get-ChildItem -LiteralPath $directory -Filter '*.dll' -File | ForEach-Object {
        $dependencyFiles[$_.Name.ToLowerInvariant()] = $_.FullName
    }
}

$queue = [System.Collections.Generic.Queue[string]]::new()
foreach ($name in @('moonlight_webrtc.exe', 'moonlight_webrtc_tray.exe')) {
    $source = Join-Path $buildDirectory $name
    Copy-Item -LiteralPath $source -Destination (Join-Path $stageDirectory $name)
    $queue.Enqueue($source)
}

$resolved = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
while ($queue.Count -gt 0) {
    $binary = $queue.Dequeue()
    foreach ($dependency in Get-PeDependents -Dumpbin $dumpbin -Path $binary) {
        $key = $dependency.ToLowerInvariant()
        if ($dependencyFiles.ContainsKey($key)) {
            if ($resolved.Add($key)) {
                $source = $dependencyFiles[$key]
                Copy-Item -LiteralPath $source -Destination (Join-Path $stageDirectory $dependency)
                $queue.Enqueue($source)
            }
        } elseif (-not (Test-SystemOrVcRuntime $dependency)) {
            throw "Unresolved non-system runtime dependency '$dependency' required by '$binary'."
        }
    }
}

Copy-Item -LiteralPath $vcRedist -Destination (Join-Path $stageDirectory 'VC_redist.x64.exe')

& $iscc ("/DSourceRoot={0}" -f $repositoryRoot) ("/DStageDir={0}" -f $stageDirectory) ("/DOutputDir={0}" -f $outputDirectory) $issPath
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup compilation failed with exit code $LASTEXITCODE."
}

$installer = Join-Path $outputDirectory 'MoonlightWebRTC-Setup.exe'
if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
    throw "Inno Setup did not produce the expected installer: $installer"
}

Write-Host "Installer created: $installer"
Write-Host 'Staged runtime files:'
Get-ChildItem -LiteralPath $stageDirectory -File | Sort-Object Name | ForEach-Object { Write-Host " - $($_.Name)" }
