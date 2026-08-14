[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ExecutablePath,
    [string]$DataDirectory = (Join-Path $env:LOCALAPPDATA 'MoonlightWebRTC')
)

$ErrorActionPreference = 'Stop'
$serviceName = 'MoonlightWebRTCGateway'
$displayName = 'Moonlight WebRTC Gateway'

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated PowerShell window.'
}

$executable = (Resolve-Path -LiteralPath $ExecutablePath).Path
if (-not (Test-Path -LiteralPath $DataDirectory -PathType Container)) {
    throw "Gateway data directory does not exist: $DataDirectory"
}
$dataDirectory = (Resolve-Path -LiteralPath $DataDirectory).Path

if (Get-Service -Name $serviceName -ErrorAction SilentlyContinue) {
    throw "The $serviceName service already exists. Run uninstall-service.ps1 first."
}

$binaryPath = '"{0}" --service --source=moonlight --data-dir="{1}"' -f $executable, $dataDirectory
try {
    New-Service -Name $serviceName -DisplayName $displayName -BinaryPathName $binaryPath -StartupType Automatic -Description 'Moonlight WebRTC Gateway service.'
    & cmd.exe /d /c ('sc.exe config "{0}" obj= "NT AUTHORITY\LocalService" password= ""' -f $serviceName) | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to configure $serviceName to run as LocalService"
    }

    # Scope access to this service SID, not every LocalService process.
    & sc.exe sidtype $serviceName unrestricted | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to enable the service SID for $serviceName"
    }
    & icacls.exe $dataDirectory /grant ("NT SERVICE\{0}:(OI)(CI)M" -f $serviceName) /T /C | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to grant $serviceName access to $dataDirectory"
    }
} catch {
    & sc.exe delete $serviceName | Out-Null
    throw
}

Write-Host "Installed $displayName using LocalService and the explicit data directory."
