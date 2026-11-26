; HS2 Port installer for Windows 7 SP1 to 11 (64-bit): the build tools\dist\dist-windows.ps1 makes, which runs on all
; of them (HS2_WIN7, plugin\CMakeLists.txt). Built by tools\dist\installer-windows.ps1, which passes:
;   /DAppVersion=0.6.1  /DStageDir=<the Windows stage folder>  /DOutputDir=...
; Needs Inno Setup 6 (6.3 or later; its setups run on Windows 7 and later).

#ifndef AppVersion
  #error AppVersion is not defined (run tools\dist\installer-windows.ps1)
#endif

[Setup]
; The AppId stays the same for every version, so a new version replaces the old one.
AppId={{6F1D2C8A-4B7E-4F3A-9C51-2E8D7B0A1F63}
AppName=HS2 Port
AppVersion={#AppVersion}
AppVerName=HS2 Port {#AppVersion}
AppPublisher=HS2 Port
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\HS2 Port
DefaultGroupName=HS2 Port
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=admin
MinVersion=6.1sp1
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
OutputDir={#OutputDir}
OutputBaseFilename=HS2-Port-{#AppVersion}-Windows-Setup
Compression=lzma2/max
SolidCompression=yes
UninstallDisplayName=HS2 Port {#AppVersion}
UninstallDisplayIcon={app}\HS2 Port.exe
SetupLogging=yes
CloseApplications=yes

[Types]
Name: "full"; Description: "VST3 plug-in and standalone application"
Name: "custom"; Description: "Choose what to install"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plug-in (Common Files\VST3)"; Types: full custom
Name: "standalone"; Description: "Standalone application"; Types: full custom

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut for the standalone application"; Components: standalone; Flags: unchecked

[InstallDelete]
; The old plug-in bundle goes first, so a bundle from an earlier version leaves nothing behind.
Type: filesandordirs; Name: "{commoncf64}\VST3\HS2 Port.vst3"; Components: vst3

[Files]
Source: "{#StageDir}\HS2 Port.vst3\*"; DestDir: "{commoncf64}\VST3\HS2 Port.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\HS2 Port.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion

[Icons]
Name: "{group}\HS2 Port"; Filename: "{app}\HS2 Port.exe"; Components: standalone
Name: "{group}\Uninstall HS2 Port"; Filename: "{uninstallexe}"
Name: "{autodesktop}\HS2 Port"; Filename: "{app}\HS2 Port.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\HS2 Port.exe"; Description: "Start HS2 Port"; Components: standalone; Flags: nowait postinstall skipifsilent unchecked

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\HS2 Port.vst3"; Components: vst3

[Messages]
FinishedLabel=Setup has installed HS2 Port on this computer.%n%nHS2 Port plays your own Hypersonic 2 content. It finds the content folder of an installed Hypersonic 2; otherwise it uses C:\Hypersonic 2 Content, or choose the folder with "Choose content folder..." in its Settings. Let your DAW rescan its plug-ins to find the VST3.
