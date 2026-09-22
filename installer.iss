; Game Stash — Inno Setup script
; Builds a proper Windows installer: Start Menu shortcut, optional Desktop
; shortcut, uninstaller in "Apps & features", and a Program Files install.
;
; Requires Inno Setup 6: https://jrsoftware.org/isinfo.php
; Build with: build_dist.bat first (to populate dist\GameStash-<ver>-win64),
; then: "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer.iss

#define MyAppName "Game Stash"
; Version is read from the built exe's ProductVersion, which comes from
; version.h - so the installer can never disagree with the app's update check.
; Override with: ISCC /DMyAppVersion=x.y.z installer.iss
#ifndef MyAppVersion
  #define MyAppVersion GetStringFileInfo(AddBackslash(SourcePath) + "build\GameStash.exe", "ProductVersion")
#endif
#if MyAppVersion == ""
  #error Could not read ProductVersion from build\GameStash.exe - build the app first
#endif
#define MyAppPublisher "Aman Singh"
#define MyAppExeName "GameStash.exe"
#define SourceDir "dist\GameStash-" + MyAppVersion + "-win64"

[Setup]
AppId={{6E6A8E2B-6E9D-4B7B-9E2E-3F6C6F8A5B10}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
; Icon shown on setup.exe itself and in the wizard. The uninstall entry
; above reads the icon embedded in the built exe, so both stay in sync.
SetupIconFile=assets\gamestash.ico
OutputDir=dist
OutputBaseFilename=GameStash-{#MyAppVersion}-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; Per-user install by default so it doesn't need admin rights;
; switch to "admin" + non-"lowest" below if you want a machine-wide install.
PrivilegesRequired=lowest
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Files]
Source: "assets\gamestash.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "oauth_config.json,debug.log,*.db,*.pdb,*.ilk"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
