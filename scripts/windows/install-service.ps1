[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ExecutablePath,
    [string]$LegacyDataDirectory = $(
        if ($env:LOCALAPPDATA) {
            Join-Path $env:LOCALAPPDATA 'MoonlightWebRTC'
        }
    )
)

$ErrorActionPreference = 'Stop'
$serviceName = 'MoonlightWebRTCGateway'
$displayName = 'Moonlight WebRTC Gateway'
$programData = [Environment]::GetFolderPath([Environment+SpecialFolder]::CommonApplicationData)
if ([string]::IsNullOrWhiteSpace($programData)) {
    throw 'Unable to resolve the Windows ProgramData known folder.'
}
$machineDataDirectory = Join-Path $programData 'MoonlightWebRTC'

function Set-ServiceDataAcl {
    param([Parameter(Mandatory = $true)][string]$Path)

    # A protected DACL prevents inherited ProgramData access from exposing the
    # Moonlight private key. Administrators and SYSTEM retain recovery access;
    # the service SID receives only the modify access required at runtime.
    $inheritance = [Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'
    $propagation = [Security.AccessControl.PropagationFlags]::None
    $allow = [Security.AccessControl.AccessControlType]::Allow
    $administrators = [Security.Principal.SecurityIdentifier]'S-1-5-32-544'
    $system = [Security.Principal.SecurityIdentifier]'S-1-5-18'
    $serviceSid = [Security.Principal.NTAccount]::new("NT SERVICE\$serviceName")

    $items = @((Get-Item -LiteralPath $Path -Force))
    $items += Get-ChildItem -LiteralPath $Path -Force -Recurse
    foreach ($item in $items) {
        $acl = if ($item.PSIsContainer) {
            [Security.AccessControl.DirectorySecurity]::new()
        } else {
            [Security.AccessControl.FileSecurity]::new()
        }
        $acl.SetAccessRuleProtection($true, $false)
        $rights = [Security.AccessControl.FileSystemRights]::FullControl
        $itemInheritance = if ($item.PSIsContainer) {
            $inheritance
        } else {
            [Security.AccessControl.InheritanceFlags]::None
        }
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
                $administrators, $rights, $itemInheritance, $propagation, $allow))
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
                $system, $rights, $itemInheritance, $propagation, $allow))
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
                $serviceSid, [Security.AccessControl.FileSystemRights]::Modify,
                $itemInheritance, $propagation, $allow))
        Set-Acl -LiteralPath $item.FullName -AclObject $acl
    }
}

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated PowerShell window.'
}

$executable = (Resolve-Path -LiteralPath $ExecutablePath).Path
$service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
$restartService = $service -and $service.Status -ne 'Stopped'
if ($restartService) {
    Stop-Service -Name $serviceName -ErrorAction Stop
    $service.WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped, [TimeSpan]::FromSeconds(30))
}

$createdService = $false
try {
    # Migrate only a source explicitly associated with the invoking interactive
    # account. The native helper validates the complete identity before copying,
    # stages the copy beside ProgramData, and never modifies the legacy source.
    $legacyExists = $LegacyDataDirectory -and (Test-Path -LiteralPath $LegacyDataDirectory -PathType Container)
    if ($legacyExists) {
        $legacyDataDirectory = (Resolve-Path -LiteralPath $LegacyDataDirectory).Path
        & $executable --source=moonlight "--data-dir=$machineDataDirectory" "--migrate-data-from=$legacyDataDirectory"
        if ($LASTEXITCODE -ne 0) {
            throw 'Gateway data migration failed; the service configuration was not changed.'
        }
    } elseif (Test-Path -LiteralPath $machineDataDirectory -PathType Container) {
        $machineDataIsEmpty = -not (Get-ChildItem -LiteralPath $machineDataDirectory -Force | Select-Object -First 1)
        if ($machineDataIsEmpty) {
            # A newly-created empty ProgramData directory is a supported fresh
            # installation. Its ACL is installed below before the service starts.
        } else {
            # Validate an existing ProgramData installation rather than silently
            # accepting an incomplete directory or regenerating its identity.
            & $executable --source=moonlight "--data-dir=$machineDataDirectory" "--migrate-data-from=$machineDataDirectory"
            if ($LASTEXITCODE -ne 0) {
                throw 'Existing ProgramData Gateway data is invalid; refusing to configure the service.'
            }
        }
    } else {
        New-Item -ItemType Directory -Path $machineDataDirectory | Out-Null
    }

    if (-not $service) {
        New-Service -Name $serviceName -DisplayName $displayName -BinaryPathName 'placeholder' -StartupType Automatic -Description 'Moonlight WebRTC Gateway service.'
        $createdService = $true
    }

    & sc.exe sidtype $serviceName unrestricted | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to enable the service SID for $serviceName"
    }
    Set-ServiceDataAcl -Path $machineDataDirectory

    $serviceCommand = '"{0}" --service --source=moonlight' -f $executable
    $serviceInstance = Get-CimInstance -ClassName Win32_Service -Filter "Name='$serviceName'"
    if (-not $serviceInstance) {
        throw "Unable to find $serviceName for LocalService configuration"
    }
    $changeResult = Invoke-CimMethod -InputObject $serviceInstance -MethodName Change -Arguments @{
        PathName = $serviceCommand
        StartMode = 'Automatic'
        StartName = 'NT AUTHORITY\LocalService'
        StartPassword = ''
    }
    if ($changeResult.ReturnValue -ne 0) {
        throw "Unable to configure $serviceName to run as LocalService. Win32_Service.Change ReturnValue: $($changeResult.ReturnValue)"
    }
} catch {
    if ($createdService) {
        & sc.exe delete $serviceName | Out-Null
    }
    if ($restartService) {
        Start-Service -Name $serviceName -ErrorAction SilentlyContinue
    }
    throw
}

Write-Host "Installed $displayName using LocalService and $machineDataDirectory."
Write-Host 'The service command line uses ProgramData; the legacy LocalAppData directory was not deleted.'
