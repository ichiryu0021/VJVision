; =============================================================================
; VJVision 2.2.2 — Windows Installer Script (Inno Setup 6)
; Build: ISCC.exe VJVision_installer.iss
;   (typical path: "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" or
;    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe")
; =============================================================================

#define MyAppName       "VJVision"
#define MyAppVersion    "2.2.2"
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
; Per-machine install into C:\Program Files\VJVision (requires UAC elevation).
; Program files here are read-only at runtime, so the app stores all user data
; (prefs, fingerprint DB, covers, standby) in the user's Documents folder:
;     %USERPROFILE%\Documents\VJVision_data
; The portable ZIP instead keeps data beside the exe — the installer writes an
; "installed.flag" marker next to the exe (see CurStepChanged) that tells the
; app which data-location mode to use, independent of process elevation.
PrivilegesRequired=admin
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes

; --- Architecture ---
; x64compatible = native x64 plus ARM64 Windows running x64 via emulation.
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible

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
; Both optional shortcuts are pre-checked by default (venue machines want the
; desktop icon and auto-start on boot); users can still untick either one.
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "startupicon"; Description: "Launch VJVision when Windows starts"; GroupDescription: "Additional shortcuts:"

[Files]
; --- Main exe + Qt runtime + FFmpeg (everything from deploy/) ---
; NOTE: We put them directly in {app}, NOT {app}\VJVision\, because
; windeployqt generates a flat layout: exe, Qt6Core.dll, qml/, platforms/, etc.
Source: "{#MyAppSourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon
Name: "{autostartup}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: startupicon

[Run]
; runascurrentuser: launch the app NON-elevated as the interactive user.
; Without it the post-install launch inherits the installer's elevated token
; and misclassifies the read-only Program Files folder as writable, stranding
; prefs/DB in {app}\data instead of Documents\VJVision_data.
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent runascurrentuser

; =============================================================================
; Uninstall — ask whether to keep user data. Installed-edition data lives in
; Documents\VJVision_data (db, prefs, covers, standby), NOT in the app folder.
; =============================================================================
[UninstallDelete]
; Data is outside {app}, so it is removed (after confirmation) via the code
; section below rather than a static UninstallDelete entry.

[Code]
// Fingerprint DB is regenerable data (re-analyze the music library), so on
// explicit confirmation we delete it permanently — no recycle-bin handling.
var
  KeepUserData: Boolean;

procedure InitializeWizard;
begin
  KeepUserData := True;
end;

// Write the installed-edition marker next to the exe. The app treats the
// presence of this file as authoritative: user data then always resolves to
// Documents\VJVision_data regardless of the process's privilege level or a
// custom install drive. The portable ZIP never contains this file.
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    SaveStringToFile(ExpandConstant('{app}\installed.flag'),
      'VJVision installed edition. This marker makes the app store data in ' +
      'Documents\VJVision_data.' + #13#10, False);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: string;
  LegacyDir: string;
begin
  if CurUninstallStep = usUninstall then
  begin
    if MsgBox('Do you want to remove your VJVision data?' + #13#10 + #13#10 +
              '  Location: Documents\VJVision_data' + #13#10 +
              '  (Fingerprint database, settings, cover cache)' + #13#10 + #13#10 +
              'Click "No" to keep it for future installations or to move your ' +
              'library to another machine.',
              mbConfirmation, MB_YESNO) = IDYES then
      KeepUserData := False
    else
      KeepUserData := True;
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    // Marker is created by install code (not in the uninstall log), so it
    // must be removed explicitly regardless of the keep-data choice.
    DeleteFile(ExpandConstant('{app}\installed.flag'));
    if not KeepUserData then
    begin
      // Current installed-edition data folder.
      DataDir := ExpandConstant('{userdocs}\VJVision_data');
      if DirExists(DataDir) then
        DelTree(DataDir, True, True, True);
      // Legacy cleanup: very old builds kept data next to the exe.
      LegacyDir := ExpandConstant('{app}\data');
      if DirExists(LegacyDir) then
        DelTree(LegacyDir, True, True, True);
    end;
  end;
end;
