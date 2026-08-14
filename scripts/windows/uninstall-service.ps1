[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$serviceName = 'MoonlightWebRTCGateway'

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated PowerShell window.'
}

$service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if (-not $service) {
    Write-Host "$serviceName is not installed."
    exit 0
}

if ($service.Status -ne 'Stopped') {
    Stop-Service -Name $serviceName -ErrorAction Stop
    $service.WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped, [TimeSpan]::FromSeconds(30))
}

& sc.exe delete $serviceName | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Unable to remove $serviceName"
}
Write-Host "Removed $serviceName. Gateway data and Sunshine pairing were not changed."
