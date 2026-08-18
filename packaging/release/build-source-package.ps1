[CmdletBinding()]
param(
    [string]$DistributionDirectory
)

$ErrorActionPreference = 'Stop'

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $output = & git -C $WorkingDirectory @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') failed with exit code $LASTEXITCODE."
    }
    return @($output)
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$submodulePath = 'third_party/moonlight-common-c'
$submoduleDirectory = Join-Path $repositoryRoot $submodulePath
if ([string]::IsNullOrWhiteSpace($DistributionDirectory)) {
    $DistributionDirectory = Join-Path $repositoryRoot 'dist'
}
$DistributionDirectory = [System.IO.Path]::GetFullPath($DistributionDirectory)
$artifact = Join-Path $DistributionDirectory 'MoonlightWebRTC-Source.tar.gz'

if (-not (Test-Path -LiteralPath (Join-Path $repositoryRoot '.git'))) {
    throw "The repository Git metadata is missing: $repositoryRoot"
}
if (-not (Test-Path -LiteralPath $submoduleDirectory -PathType Container)) {
    throw "The moonlight-common-c submodule is missing or not initialized: $submoduleDirectory"
}
if (-not (Test-Path -LiteralPath (Join-Path $submoduleDirectory '.git'))) {
    throw "The moonlight-common-c submodule Git metadata is missing: $submoduleDirectory"
}

$projectCommit = (Invoke-Git -WorkingDirectory $repositoryRoot -Arguments @('rev-parse', 'HEAD')).Trim()
$treeEntry = @(Invoke-Git -WorkingDirectory $repositoryRoot -Arguments @('ls-tree', 'HEAD', '--', $submodulePath))
if ($treeEntry.Count -ne 1 -or $treeEntry[0] -notmatch '^160000 commit ([0-9a-f]{40})\s+') {
    throw 'The current project commit does not contain a pinned moonlight-common-c submodule.'
}
$expectedSubmoduleCommit = $Matches[1]
$submoduleCommit = (Invoke-Git -WorkingDirectory $submoduleDirectory -Arguments @('rev-parse', 'HEAD')).Trim()
if ($submoduleCommit -ne $expectedSubmoduleCommit) {
    throw "moonlight-common-c is checked out at $submoduleCommit; expected $expectedSubmoduleCommit."
}
if ((Invoke-Git -WorkingDirectory $submoduleDirectory -Arguments @('status', '--porcelain')).Count -ne 0) {
    throw 'moonlight-common-c has local changes; source packaging requires the exact pinned checkout.'
}

$rootFiles = Invoke-Git -WorkingDirectory $repositoryRoot -Arguments @('ls-tree', '-r', '--name-only', 'HEAD')
$forbiddenPath = $rootFiles | Where-Object {
    $_ -match '(^|/)AGENTS\.md$' -or $_ -match '^(dist|build[^/]*|\.tizen-install|tizen/(Release|Debug|\.buildResult))/' -or
    $_ -match '(?i)\.(p12|pfx|key|pem)$' -or $_ -match '(^|/)(ProgramData|LocalAppData)(/|$)'
} | Select-Object -First 1
if ($forbiddenPath) {
    throw "Refusing to package forbidden tracked path: $forbiddenPath"
}

$tar = Get-Command tar.exe -ErrorAction SilentlyContinue
if (-not $tar) {
    $tar = Get-Command tar -ErrorAction SilentlyContinue
}
if (-not $tar) {
    throw 'A tar implementation is required to create MoonlightWebRTC-Source.tar.gz.'
}

$temporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("MoonlightWebRTC-source-" + [guid]::NewGuid().ToString('N'))
$rootArchive = Join-Path $temporaryDirectory 'project.tar'
$submoduleArchive = Join-Path $temporaryDirectory 'moonlight-common-c.tar'
$stageDirectory = Join-Path $temporaryDirectory 'stage'
$archiveRoot = 'MoonlightWebRTC'

New-Item -ItemType Directory -Path $DistributionDirectory, $stageDirectory -Force | Out-Null
try {
    & git -C $repositoryRoot archive --format=tar --prefix="$archiveRoot/" --output=$rootArchive $projectCommit
    if ($LASTEXITCODE -ne 0) {
        throw "git archive for the project failed with exit code $LASTEXITCODE."
    }
    & $tar.Source -xf $rootArchive -C $stageDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "Extracting the project source archive failed with exit code $LASTEXITCODE."
    }

    & git -C $submoduleDirectory archive --format=tar --prefix="$archiveRoot/$submodulePath/" --output=$submoduleArchive $submoduleCommit
    if ($LASTEXITCODE -ne 0) {
        throw "git archive for moonlight-common-c failed with exit code $LASTEXITCODE."
    }
    & $tar.Source -xf $submoduleArchive -C $stageDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "Extracting the moonlight-common-c source archive failed with exit code $LASTEXITCODE."
    }

    $sourceInfo = @(
        'Moonlight WebRTC source release',
        "Project commit: $projectCommit",
        "moonlight-common-c commit: $submoduleCommit"
    )
    Set-Content -LiteralPath (Join-Path $stageDirectory "$archiveRoot\SOURCE_INFO.txt") -Value $sourceInfo -Encoding ascii

    if (Test-Path -LiteralPath $artifact) {
        Remove-Item -LiteralPath $artifact -Force
    }
    & $tar.Source -czf $artifact --options 'gzip:!timestamp' --format=ustar --uid 0 --gid 0 --numeric-owner --mtime '1970-01-01 00:00:00Z' -C $stageDirectory $archiveRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Creating the source archive failed with exit code $LASTEXITCODE."
    }
} finally {
    Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Source release created: $artifact"
