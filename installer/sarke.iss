; Sarke installer (Inno Setup 6). Build it with tools\make_installer.ps1, which builds the release
; executables first and writes dist\SarkeSetup-<version>.exe.
;
; What a PC needs for Sarke to just work, all in here:
;   - sarke.exe (the tray app) and the apps it runs (kvm_host.exe, kvm_client.exe), static: no runtimes
;   - the fonts its windows use (assets\fonts)
;   - the Virtual Display Driver for desk mode (MikeTheTech's signed IddCx driver, MIT) and its settings
;     file, writable by users so Sarke can add the other PC's resolution
;   - Windows Firewall rules for the local network on Private and Domain networks (TCP 50500-50502,
;     UDP 50503 discovery); a Public network stays closed
;   - optionally a Microsoft Defender exclusion (off by default: it also stops Defender scanning there)
;   - Start menu entry, optional desktop shortcut, optionally start with Windows
; Per-user settings and logs live in %LOCALAPPDATA%\Sarke (kept on uninstall).

; ISCC /DBuild=<folder> packs the programs of another build folder (e.g. a fresh clone using an existing build).
#ifndef Build
  #define Build "..\build\x64-release"
#endif
#define AppVersion GetVersionNumbersString(Build + "\tools\sarke.exe")

[Setup]
AppId={{2E907321-CE0F-4011-A70C-D5937B9EDCA5}
AppName=Sarke
AppVersion={#AppVersion}
AppVerName=Sarke {#AppVersion}
AppPublisher=Sarke
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Sarke setup
DefaultDirName={autopf}\Sarke
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=SarkeSetup-{#AppVersion}
SetupIconFile=..\docs\brand\sarke.ico
UninstallDisplayIcon={app}\sarke.exe
UninstallDisplayName=Sarke
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows.Graphics.Capture of single windows needs Windows 10 2004 or later.
MinVersion=10.0.19041
CloseApplications=yes
RestartApplications=no

[Tasks]
; Starting with Windows and the Defender exclusion are the user's choice: off by default (roadmap P1.11).
Name: autostart; Description: "Start Sarke when Windows starts"; Flags: unchecked
Name: desktopicon; Description: "Create a desktop shortcut"; Flags: unchecked
Name: defender; Description: "Add a Microsoft Defender exclusion for Sarke's folder. Only if Defender blocks Sarke: its keyboard and mouse hooks can look suspicious to Defender, but an exclusion also means Defender no longer scans anything in that folder."; Flags: unchecked

[Files]
Source: "{#Build}\tools\sarke.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\host_a\kvm_host.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\client_b\kvm_client.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Build}\tools\sarke_driver.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\assets\fonts\*"; DestDir: "{app}\assets\fonts"; Flags: ignoreversion
Source: "..\tools\vdd_modes.ps1"; DestDir: "{app}\tools"; Flags: ignoreversion
Source: "vdd\mttvdd.inf"; DestDir: "{app}\driver"; Flags: ignoreversion
Source: "vdd\MttVDD.dll"; DestDir: "{app}\driver"; Flags: ignoreversion
Source: "vdd\MttVDD.cat"; DestDir: "{app}\driver"; Flags: ignoreversion
Source: "vdd\LICENSE.txt"; DestDir: "{app}\licenses"; DestName: "VirtualDisplayDriver-LICENSE.txt"; Flags: ignoreversion
; Sarke's license and the licenses of everything compiled into it.
Source: "..\LICENSE"; DestDir: "{app}\licenses"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\NOTICE"; DestDir: "{app}\licenses"; DestName: "NOTICE.txt"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}\licenses"; Flags: ignoreversion
; The driver reads its resolutions from here; Sarke adds the other PC's size, so users may write it.
; An existing file (the user's own list) is kept.
Source: "vdd\vdd_settings.xml"; DestDir: "C:\VirtualDisplayDriver"; Permissions: authusers-modify; Flags: onlyifdoesntexist uninsneveruninstall

[Dirs]
Name: "C:\VirtualDisplayDriver"; Permissions: authusers-modify; Flags: uninsneveruninstall

[Icons]
Name: "{autoprograms}\Sarke"; Filename: "{app}\sarke.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\Sarke"; Filename: "{app}\sarke.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Sarke"; ValueData: """{app}\sarke.exe"" --background"; Flags: uninsdeletevalue; Tasks: autostart

[Run]
; Windows Firewall: Sarke's ports only, from the local network only, and only on Private and Domain
; networks: on a Public one (an airport, a cafe) nothing gets in. A phone hotspot that Windows calls
; Public has to be made Private first; the launcher says how. No rule for the programs themselves:
; that would open every port they listen on. Old rules go first so a reinstall doesn't add twins
; (and an upgrade from 1.1 loses its any-profile and program rules).
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Sarke"""; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Sarke"" dir=in action=allow protocol=TCP localport=50500-50502 remoteip=localsubnet profile=private,domain"; Flags: runhidden; StatusMsg: "Allowing Sarke through Windows Firewall..."
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Sarke"" dir=in action=allow protocol=UDP localport=50503 remoteip=localsubnet profile=private,domain"; Flags: runhidden
Filename: "{app}\sarke.exe"; Description: "Start Sarke now"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{app}\sarke.exe"; Parameters: "--quit"; Flags: runhidden; RunOnceId: "QuitSarke"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Sarke"""; Flags: runhidden; RunOnceId: "Firewall"
Filename: "{app}\sarke_driver.exe"; Parameters: "remove"; Flags: runhidden; RunOnceId: "VirtualDisplay"
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -Command ""Remove-MpPreference -ExclusionPath '{app}' -ErrorAction SilentlyContinue"""; Flags: runhidden; RunOnceId: "Defender"

[Code]
var
  DriverRestart: Boolean;

// A running Sarke holds its files: ask it to quit (it detaches the virtual display), then make sure.
procedure StopSarke();
var
  Code: Integer;
begin
  if FileExists(ExpandConstant('{app}\sarke.exe')) then
    Exec(ExpandConstant('{app}\sarke.exe'), '--quit', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Sleep(1500);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM kvm_host.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM kvm_client.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopSarke();
  Result := '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  // Before the files land, or Defender may take kvm_host.exe away as it is written.
  if (CurStep = ssInstall) and WizardIsTaskSelected('defender') then
    Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
         '-NoProfile -ExecutionPolicy Bypass -Command "Add-MpPreference -ExclusionPath ''' + ExpandConstant('{app}') + '''"',
         '', SW_HIDE, ewWaitUntilTerminated, Code);
  if CurStep = ssPostInstall then begin
    WizardForm.StatusLabel.Caption := 'Installing the virtual display driver (desk mode)...';
    if Exec(ExpandConstant('{app}\sarke_driver.exe'), 'install "' + ExpandConstant('{app}\driver') + '"',
            '', SW_HIDE, ewWaitUntilTerminated, Code) then begin
      if Code = 3010 then
        DriverRestart := True
      else if Code <> 0 then
        SuppressibleMsgBox('The virtual display driver could not be installed (code ' + IntToStr(Code) + ').' + #13#10 +
                           'Sarke works without it, but desk mode (the other PC''s screen as one more monitor) needs it.',
                           mbInformation, MB_OK, IDOK);
    end;
  end;
end;

function NeedRestart(): Boolean;
begin
  Result := DriverRestart;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Code: Integer;
begin
  if CurUninstallStep = usUninstall then begin
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM kvm_host.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM kvm_client.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  end;
end;
