#ifndef SourceRoot
  #define SourceRoot "..\\.."
#endif
#ifndef StageDir
  #define StageDir SourceRoot + "\\dist\\windows\\stage"
#endif
#ifndef OutputDir
  #define OutputDir SourceRoot + "\\dist\\windows"
#endif

[Setup]
AppId={{89ABF3E2-D0A7-49C1-8D7C-65B2E9CCCD28}
AppName=Moonlight WebRTC
AppVersion=1.0.0
AppPublisher=Moonlight WebRTC
DefaultDirName={autopf}\Moonlight WebRTC
DefaultGroupName=Moonlight WebRTC
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=MoonlightWebRTC-Setup
SetupIconFile={#SourceRoot}\src\tray\resources\moonlight.ico
UninstallDisplayIcon={app}\moonlight_webrtc_tray.exe
VersionInfoVersion=1.0.0.0
VersionInfoProductVersion=1.0.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern

[Files]
Source: "{#StageDir}\moonlight_webrtc.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\moonlight_webrtc_tray.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\VC_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
Source: "{#SourceRoot}\scripts\windows\install-service.ps1"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\Moonlight WebRTC"; Filename: "{app}\moonlight_webrtc_tray.exe"; IconFilename: "{app}\moonlight_webrtc_tray.exe"

[Run]
Filename: "{app}\moonlight_webrtc_tray.exe"; Description: "Launch Moonlight WebRTC"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallDelete]
Type: dirifempty; Name: "{group}"

[Code]
const
  ServiceName = 'MoonlightWebRTCGateway';
  FirewallRuleName = 'Moonlight WebRTC Gateway';

function PowerShellPath(): String;
begin
  Result := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
end;

function RunPowerShell(const Parameters, FailureMessage: String): Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(PowerShellPath(), Parameters, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  if (not Result) or (ResultCode <> 0) then begin
    Log(FailureMessage + ' PowerShell exit code: ' + IntToStr(ResultCode) +
      '. Command: ' + Parameters);
    Result := False;
  end;
end;

function StopGatewayService(): Boolean;
var
  ResultCode: Integer;
  Script: String;
begin
  if not Exec(ExpandConstant('{sys}\sc.exe'), 'query ' + ServiceName, '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode) then begin
    Log('Unable to invoke sc.exe query for ' + ServiceName + '.');
    Result := False;
    exit;
  end;
  Log('sc.exe query ' + ServiceName + ' exit code: ' + IntToStr(ResultCode));
  if ResultCode = 1060 then begin
    Log(ServiceName + ' is not installed; no service stop is required.');
    Result := True;
    exit;
  end;
  if ResultCode <> 0 then begin
    Log('Unable to query ' + ServiceName + '. sc.exe exit code: ' + IntToStr(ResultCode));
    Result := False;
    exit;
  end;

  if not Exec(ExpandConstant('{sys}\sc.exe'), 'stop ' + ServiceName, '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode) then begin
    Log('Unable to invoke sc.exe stop for ' + ServiceName + '.');
    Result := False;
    exit;
  end;
  Log('sc.exe stop ' + ServiceName + ' exit code: ' + IntToStr(ResultCode));
  if ResultCode = 1062 then begin
    Log(ServiceName + ' is already stopped.');
    Result := True;
    exit;
  end;
  if (ResultCode <> 0) and (ResultCode <> 1061) then begin
    Log('Unable to stop ' + ServiceName + '. sc.exe exit code: ' + IntToStr(ResultCode));
    Result := False;
    exit;
  end;

  Script := 'try { $service = Get-Service -Name ''' + ServiceName + ''' -ErrorAction Stop } ' +
    'catch [System.InvalidOperationException] { exit 0 }; ' +
    'if ($service.Status -ne ''Stopped'') { ' +
    '$service.WaitForStatus([System.ServiceProcess.ServiceControllerStatus]::Stopped, [TimeSpan]::FromSeconds(30)) }; ' +
    'if ($service.Status -ne ''Stopped'') { Write-Error ''Service did not reach Stopped within 30 seconds.''; exit 1 }; exit 0';
  Result := RunPowerShell('-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "' + Script + '"',
    'Unable to wait for the Moonlight WebRTC Gateway service to stop.');
end;

procedure StopTrayForOriginalUser();
var
  ResultCode: Integer;
begin
  { A missing tray process returns a nonzero taskkill code and is harmless. }
  ExecAsOriginalUser(ExpandConstant('{sys}\taskkill.exe'), '/f /im moonlight_webrtc_tray.exe', '',
    SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure StopTrayForUninstall();
var
  ResultCode: Integer;
begin
  { A missing tray process returns a nonzero taskkill code and is harmless. }
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im moonlight_webrtc_tray.exe', '',
    SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function ConfigureService(): Boolean;
var
  Parameters: String;
begin
  Parameters := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' +
    ExpandConstant('{tmp}\install-service.ps1') + '" -ExecutablePath "' +
    ExpandConstant('{app}\moonlight_webrtc.exe') + '"';
  Result := RunPowerShell(Parameters,
    'Moonlight WebRTC Gateway service configuration failed. The installer cannot continue.');
end;

function ConfigureFirewall(): Boolean;
var
  ResultCode: Integer;
  Parameters: String;
begin
  Exec(ExpandConstant('{sys}\netsh.exe'),
    'advfirewall firewall delete rule name="' + FirewallRuleName + '"', '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode);
  Parameters := 'advfirewall firewall add rule name="' + FirewallRuleName +
    '" dir=in action=allow program="' + ExpandConstant('{app}\moonlight_webrtc.exe') +
    '" enable=yes profile=all remoteip=localsubnet';
  Result := Exec(ExpandConstant('{sys}\netsh.exe'), Parameters, '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
  if not Result then
    Log('Unable to create the all-profile, LocalSubnet-restricted Moonlight WebRTC firewall rule. Exit code: ' + IntToStr(ResultCode));
end;

function ConfigureTrayAutostart(): Boolean;
var
  ResultCode: Integer;
  RegistryFile: String;
  RegistryContents: String;
begin
  RegistryFile := ExpandConstant('{tmp}\moonlight-webrtc-autostart.reg');
  RegistryContents := 'Windows Registry Editor Version 5.00' + #13#10 + #13#10 +
    '[HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run]' + #13#10 +
    '"Moonlight WebRTC"="\"' + ExpandConstant('{app}\moonlight_webrtc_tray.exe') + '\""' + #13#10;
  if not SaveStringToFile(RegistryFile, RegistryContents, False) then begin
    Log('Unable to create the Moonlight WebRTC tray autostart registry file.');
    Result := False;
    exit;
  end;
  Result := ExecAsOriginalUser(ExpandConstant('{sys}\reg.exe'), 'import "' + RegistryFile + '"', '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
  DeleteFile(RegistryFile);
  if not Result then
    Log('Unable to configure the Moonlight WebRTC tray autostart entry. Exit code: ' + IntToStr(ResultCode));
end;

procedure RemoveTrayAutostart();
var
  ResultCode: Integer;
begin
  { A missing entry returns a nonzero reg.exe code and is harmless. }
  ExecAsOriginalUser(ExpandConstant('{sys}\reg.exe'),
    'delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v "Moonlight WebRTC" /f', '',
    SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure RemoveTrayAutostartForUninstall();
var
  ResultCode: Integer;
begin
  { A missing entry returns a nonzero reg.exe code and is harmless. }
  Exec(ExpandConstant('{sys}\reg.exe'),
    'delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v "Moonlight WebRTC" /f', '',
    SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function StartGatewayService(): Boolean;
var
  Script: String;
begin
  Script := 'Start-Service -Name ''' + ServiceName + ''' -ErrorAction Stop; ' +
    '$service = Get-Service -Name ''' + ServiceName + ''' -ErrorAction Stop; ' +
    '$service.WaitForStatus([System.ServiceProcess.ServiceControllerStatus]::Running, [TimeSpan]::FromSeconds(30))';
  Result := RunPowerShell('-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "' + Script + '"',
    'Unable to start the Moonlight WebRTC Gateway service.');
end;

function InstallVcRuntime(): Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(ExpandConstant('{tmp}\VC_redist.x64.exe'), '/install /quiet /norestart', '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode) and ((ResultCode = 0) or (ResultCode = 1638) or (ResultCode = 3010));
  if not Result then
    Log('Microsoft Visual C++ x64 redistributable installation failed. Exit code: ' + IntToStr(ResultCode));
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  StopTrayForOriginalUser();
  if not StopGatewayService() then
    Result := 'Moonlight WebRTC Gateway could not be stopped. Close applications using it and try again.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    if not InstallVcRuntime() then
      RaiseException('Microsoft Visual C++ x64 redistributable installation failed.');
    if not ConfigureService() then
      RaiseException('Moonlight WebRTC Gateway service configuration failed.');
    if not ConfigureFirewall() then
      RaiseException('Moonlight WebRTC firewall configuration failed.');
    if not ConfigureTrayAutostart() then
      RaiseException('Moonlight WebRTC tray autostart configuration failed.');
    if not StartGatewayService() then
      RaiseException('Moonlight WebRTC Gateway service could not be started.');
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then begin
    StopTrayForUninstall();
    if not StopGatewayService() then begin
      MsgBox('Moonlight WebRTC Gateway could not be stopped. The uninstaller cannot safely continue.', mbError, MB_OK);
      Abort;
    end;
    RemoveTrayAutostartForUninstall();
    Exec(ExpandConstant('{sys}\sc.exe'), 'delete ' + ServiceName, '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'),
      'advfirewall firewall delete rule name="' + FirewallRuleName + '"', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
  end;
end;
