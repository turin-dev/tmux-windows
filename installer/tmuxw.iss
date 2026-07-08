; tmuxw.iss — custom install wizard for tmuxw (Inno Setup 6).
;
; A GUI setup wizard that installs tmuxw with a license page, install-location
; picker, optional PATH registration, and an optional starter config. It ships
; both command names: `tmux` and `tmuxw`.
;
; Build (Inno Setup's ISCC must be on PATH):
;   ISCC /DAppVersion=0.1.0 installer\tmuxw.iss
; Output:
;   dist\tmuxw-<version>-setup.exe
;
; The GitHub `windows-latest` runners ship Inno Setup, so CI builds this on tag.

#ifndef AppVersion
  #define AppVersion "0.1.1"
#endif

; Where the built binaries live, relative to this script (../build by default).
#ifndef SrcDir
  #define SrcDir "..\build"
#endif
; Repo root relative to this script (for LICENSE/README/etc.).
#ifndef RepoRoot
  #define RepoRoot ".."
#endif

#define AppName    "tmuxw"
#define AppPublisher "Turin"
#define AppUrl     "https://github.com/turin-dev/tmux-windows"

[Setup]
; A stable AppId ties upgrades/uninstall to the same product across versions.
AppId={{5F2A9C31-8B4E-4D6A-9E2F-1C7A3B5D8E04}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\tmuxw
DefaultGroupName=tmuxw
DisableProgramGroupPage=yes
LicenseFile={#RepoRoot}\LICENSE
OutputDir={#RepoRoot}\dist
OutputBaseFilename=tmuxw-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\tmuxw.exe
UninstallDisplayName={#AppName} {#AppVersion}
; tmuxw is 64-bit only (ConPTY, Win10 1809+).
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
; Default to a per-user install (no UAC); allow the user to elevate if desired.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "addtopath"; \
  Description: "Add tmuxw to the PATH (use ""tmux"" and ""tmuxw"" in any terminal)"; \
  GroupDescription: "Integration:"
Name: "starterconfig"; \
  Description: "Create a starter %USERPROFILE%\.tmuxw.conf (only if none exists)"; \
  GroupDescription: "Integration:"; Flags: unchecked

[Files]
Source: "{#SrcDir}\tmuxw.exe";                DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcDir}\tmux.exe";                 DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\LICENSE";                DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\README.md";              DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "{#RepoRoot}\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\tmuxw.conf.example";     DestDir: "{app}"; Flags: ignoreversion
; Optional starter config, dropped into the home dir only when absent.
Source: "{#RepoRoot}\tmuxw.conf.example"; DestDir: "{%USERPROFILE}"; DestName: ".tmuxw.conf"; \
  Tasks: starterconfig; Flags: onlyifdoesntexist

[Icons]
Name: "{group}\tmuxw";           Filename: "{app}\tmuxw.exe"; Comment: "Start or attach a tmuxw session"
Name: "{group}\tmuxw README";    Filename: "{app}\README.md"
Name: "{group}\Uninstall tmuxw"; Filename: "{uninstallexe}"

[Registry]
; Per-user PATH entry (HKCU\Environment) — no admin needed, works for both
; install modes. The Check keeps it idempotent across re-installs/upgrades.
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; \
  ValueData: "{olddata};{app}"; Tasks: addtopath; \
  Check: NeedsAddPath(ExpandConstant('{app}'))

[Run]
Filename: "{app}\README.md"; Description: "View the README"; \
  Flags: postinstall shellexec skipifsilent unchecked

[Code]
// True when the install dir is not already present in the per-user PATH.
function NeedsAddPath(Param: string): Boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', OrigPath) then
  begin
    Result := True;
    exit;
  end;
  // Wrap both sides in ';' so we match a whole entry, not a substring.
  Result := Pos(';' + Uppercase(Param) + ';', ';' + Uppercase(OrigPath) + ';') = 0;
end;

// Strip the install dir from the per-user PATH on uninstall.
procedure RemoveFromPath(PathDir: string);
var
  OrigPath, NewPath: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', OrigPath) then
    exit;
  NewPath := ';' + OrigPath + ';';
  StringChangeEx(NewPath, ';' + PathDir + ';', ';', True);
  // Trim the sentinel semicolons we added.
  if (Length(NewPath) > 0) and (NewPath[1] = ';') then
    Delete(NewPath, 1, 1);
  if (Length(NewPath) > 0) and (NewPath[Length(NewPath)] = ';') then
    Delete(NewPath, Length(NewPath), 1);
  if NewPath <> OrigPath then
    RegWriteExpandStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', NewPath);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RemoveFromPath(ExpandConstant('{app}'));
end;
