#define AppVersion "0.2.0"
[Setup]
AppId={{8103AE92-21E5-40DC-B917-871885F0CF14}
AppName=NCO
AppVersion={#AppVersion}
AppPublisher=NCO contributors
AppPublisherURL=https://github.com/NeusByCast/NCO
AppSupportURL=https://github.com/NeusByCast/NCO/issues
AppUpdatesURL=https://github.com/NeusByCast/NCO/releases
DefaultDirName={autopf}\NCO
DefaultGroupName=NCO
AllowNoIcons=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.18362
OutputDir=..\output
OutputBaseFilename=NCO-Setup
SetupIconFile=..\assets\nco.ico
UninstallDisplayIcon={app}\NCO.exe
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
WizardResizable=yes
WizardImageBackColor=$00131A24
WizardImageFile=..\assets\wizard.bmp
WizardSmallImageFile=..\assets\wizard-small.bmp
WizardImageStretch=no
LicenseFile=..\LICENSE
CloseApplications=yes
RestartApplications=no
SetupMutex=NCOSetup
AppMutex=Local\NCORecorder
UninstallDisplayName=NCO
VersionInfoDescription=NCO screen recorder setup
VersionInfoProductName=NCO
VersionInfoProductVersion={#AppVersion}
ChangesAssociations=no
DisableWelcomePage=no

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "..\dist\NCO.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\THIRD_PARTY.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\TESTING.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\NCO"; Filename: "{app}\NCO.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\NCO"; Filename: "{app}\NCO.exe"; Tasks: desktopicon; WorkingDir: "{app}"

[Run]
Filename: "{app}\NCO.exe"; Description: "Launch NCO"; Flags: nowait postinstall skipifsilent
