; =============================================================================
; VJVision 2.0.0 — Windows Installer Script (Inno Setup 6)
; Build: "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" VJVision_installer.iss
; =============================================================================

#define MyAppName       "VJVision"
#define MyAppVersion    "2.0.0"
#define MyAppPublisher  "Ichiryu"
#define MyAppExeName    "VJVision.exe"
#define MyAppSourceDir  "deploy"

[Setup]
; --- Basic ---
AppId={{8B4E3F2A-7C5D-4E8F-9A1B-2C3D4E5F6A7B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL=https://github.com/ichiryu0021/VJVision
AppSupportURL=https://github.com/ichiryu0021/VJVision/issues
AppUpdatesURL=https://github.com/ichiryu0021/VJVision/releases

; --- Install location ---
; Install to user's local app data (no admin needed). data/ folder travels with
; the exe so the install is "portable-grade" — copy the folder to a USB stick
; and it works everywhere.
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes

; --- Architecture ---
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64

; --- Output ---
OutputDir=installer_output
OutputBaseFilename=VJVision-{#MyAppVersion}-windows-x64-installer
SetupIconFile=
Compression=lzma2/ultra
SolidCompression=yes
WizardStyle=modern

; --- Uninstall ---
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}

; --- Lang ---
[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "startupicon"; Description: "Launch VJVision when Windows starts"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
; --- Main exe + Qt runtime + FFmpeg (everything from deploy/) ---
; NOTE: We put them directly in {app}, NOT {app}\VJVision\, because
; windeployqt generates a flat layout: exe, Qt6Core.dll, qml/, platforms/, etc.
Source: "{#MyAppSourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon
Name: "{userstartup}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: startupicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

; =============================================================================
; Uninstall — ask whether to keep user data (data/ folder: db, prefs, covers)
; =============================================================================
[UninstallDelete]
; Always remove the data folder if user didn't opt to keep it.
; We use a custom code section below to prompt, not a static UninstallDelete.

[Code]
var
  KeepUserData: Boolean;

procedure InitializeWizard;
begin
  KeepUserData := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: string;
begin
  if CurUninstallStep = usUninstall then
  begin
    if MsgBox('Do you want to remove your VJVision data?' + #13#10 + #13#10 +
              '  (Fingerprint database, settings, cover cache)' + #13#10 + #13#10 +
              'Click "No" to keep your data folder for future installations, ' +
              'or if you want to take your library to another machine.',
              mbConfirmation, MB_YESNO) = IDYES then
      KeepUserData := False
    else
      KeepUserData := True;
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    DataDir := ExpandConstant('{app}\data');
    if not KeepUserData and DirExists(DataDir) then
      DelTree(DataDir, True, True, True);
  end;
end;
