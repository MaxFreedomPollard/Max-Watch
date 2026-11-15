#define MyAppName "Max Watch"
#define MyAppVersion "1.3.0"
#define MyAppPublisher "Max Freedom Pollard"
#define MyAppURL "https://github.com/MaxFreedomPollard/Max-Watch"
#define MyAppExeName "MaxWatch.exe"

[Setup]
AppId={{C60F4D72-8E11-4A9D-B531-9A2E4C801300}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
OutputDir=..\build\dist
OutputBaseFilename=Max-Watch-{#MyAppVersion}-Windows-Setup
SetupIconFile=..\resources\MaxWatch.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible arm64
ArchitecturesInstallIn64BitMode=x64compatible arm64
CloseApplications=yes
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\build\x64\Release\MaxWatch.exe"; DestDir: "{app}"; DestName: "{#MyAppExeName}"; Check: not IsArm64; Flags: ignoreversion
Source: "..\build\arm64\Release\MaxWatch.exe"; DestDir: "{app}"; DestName: "{#MyAppExeName}"; Check: IsArm64; Flags: ignoreversion
Source: "..\..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Comment: "100% Offline Motion Security Camera & AES-256 Encrypted Video Vault"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon; Comment: "100% Offline Motion Security Camera & AES-256 Encrypted Video Vault"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
