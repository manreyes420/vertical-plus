; Vertical+ for OBS Studio - Windows installer (Inno Setup 6)
;
; Installs the plugin into OBS Studio's per-machine plugin folder:
;   C:\ProgramData\obs-studio\plugins\vertical-plus\bin\64bit\vertical-plus.dll
;   C:\ProgramData\obs-studio\plugins\vertical-plus\data\...
; This folder is loaded by OBS 32+ and survives OBS updates.
;
; CI builds it with:
;   ISCC /DAppVersion=<ver> /DSourceDir=<release\Config\vertical-plus> /DOutputDir=<release> vertical-plus.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\release\RelWithDebInfo\vertical-plus"
#endif
#ifndef OutputDir
  #define OutputDir "..\release"
#endif

[Setup]
; Keep this AppId forever: it is how upgrades and the uninstaller find
; earlier installs.
AppId={{332678E0-E0B3-49F3-8433-1B9D5F658BAA}
AppName=Vertical+ for OBS
AppVersion={#AppVersion}
AppVerName=Vertical+ for OBS {#AppVersion}
AppPublisher=manreyes420
AppPublisherURL=https://github.com/manreyes420/vertical-plus
AppSupportURL=https://github.com/manreyes420/vertical-plus/issues
DefaultDirName={commonappdata}\obs-studio\plugins\vertical-plus
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=vertical-plus-{#AppVersion}-windows-x64-Installer
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; OBS holds this mutex while it is running (frontend/utility/platform-windows.cpp).
AppMutex=OBSStudioCore
CloseApplications=no
UninstallDisplayName=Vertical+ for OBS
SetupLogging=yes

[Messages]
SetupAppRunningError=OBS Studio is running.%n%nClose OBS, then click OK to continue, or Cancel to exit.
UninstallAppRunningError=OBS Studio is running.%n%nClose OBS, then click OK to continue, or Cancel to exit.
WelcomeLabel2=This installs [name/ver], a plugin that adds a vertical (9:16) canvas to OBS Studio so you can stream to mobile platforms and your normal stream at the same time.%n%nRequires OBS Studio 32 or newer. Close OBS before continuing.
FinishedLabel=Vertical+ is installed.%n%nOpen OBS Studio, then use Docks > Vertical+ and Tools > Vertical+ Settings.%n%nTip: before your first live stream, back up your scenes with Scene Collection > Export.

[Files]
Source: "{#SourceDir}\bin\64bit\vertical-plus.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "{#SourceDir}\bin\64bit\vertical-plus.pdb"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
; Only the plugin's own folder. Vertical+ settings live in your OBS profile
; folder (vertical-plus.json) and are kept.
Type: filesandordirs; Name: "{app}"

[Code]
const
  ObsUninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio';

function ObsMajorVersion(const Version: String): Integer;
var
  Dot: Integer;
begin
  Dot := Pos('.', Version);
  if Dot > 1 then
    Result := StrToIntDef(Copy(Version, 1, Dot - 1), 0)
  else
    Result := StrToIntDef(Version, 0);
end;

function InitializeSetup(): Boolean;
var
  Version: String;
  Major: Integer;
begin
  Result := True;
  { The OBS installer registers itself in the 32-bit registry view. }
  if RegQueryStringValue(HKLM32, ObsUninstallKey, 'DisplayVersion', Version) or
     RegQueryStringValue(HKLM64, ObsUninstallKey, 'DisplayVersion', Version) then
  begin
    Major := ObsMajorVersion(Version);
    if (Major > 0) and (Major < 32) then
      Result := SuppressibleMsgBox(
        'Vertical+ needs OBS Studio 32 or newer, but OBS ' + Version + ' is installed.' + #13#10#13#10 +
        'Update OBS first (Help > Check for Updates). Install Vertical+ anyway?',
        mbConfirmation, MB_YESNO, IDYES) = IDYES;
  end
  else
    Result := SuppressibleMsgBox(
      'OBS Studio was not found on this PC. Vertical+ is a plugin for OBS Studio 32 or newer.' + #13#10#13#10 +
      'Install anyway? (Choose Yes if you use a portable or custom OBS install.)',
      mbConfirmation, MB_YESNO, IDYES) = IDYES;
end;
