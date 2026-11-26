; Generated from the same portable package delivered alongside Setup.
[Setup]
AppId={{F3BD5762-DAD8-4581-9DF6-091C8B79DEF2}
AppName=FileSearcher
AppVersion={#AppVersion}
AppPublisher=Hugo Chen
DefaultDirName={localappdata}\Programs\FileSearcher
DefaultGroupName=FileSearcher
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
Compression=lzma2/fast
SolidCompression=yes
UninstallDisplayIcon={app}\filesearcher.exe
OutputBaseFilename=FileSearcher-Setup
CloseApplications=yes
RestartApplications=no

[Files]
Source: "{#PackageDir}\vc_redist.x64.exe"; Flags: dontcopy
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\FileSearcher"; Filename: "{app}\filesearcher.exe"; WorkingDir: "{app}"

[Run]
Filename: "{app}\filesearcher.exe"; Description: "Open FileSearcher"; Flags: nowait postinstall skipifsilent

[Code]
function RuntimeReady(): Boolean;
var
  Installed: Cardinal;
  Version: String;
  Actual, Required: Int64;
begin
  Result := False;
  if RegQueryDWordValue(HKLM64, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed)
    and (Installed = 1)
    and RegQueryStringValue(HKLM64, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Version', Version) then begin
    if Copy(Version, 1, 1) = 'v' then Delete(Version, 1, 1);
    if StrToVersion(Version, Actual) and StrToVersion('{#RuntimeVersion}', Required) then
      Result := ComparePackedVersion(Actual, Required) >= 0;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ExitCode: Integer;
begin
  Result := '';
  if RuntimeReady() then Exit;
  ExtractTemporaryFile('vc_redist.x64.exe');
  if not ShellExec('runas', ExpandConstant('{tmp}\vc_redist.x64.exe'),
    '/install /passive /norestart', '', SW_SHOWNORMAL, ewWaitUntilTerminated, ExitCode) then
    Result := 'The Microsoft Visual C++ runtime requires administrator approval.'
  else if ExitCode = 3010 then begin
    NeedsRestart := True;
    Result := 'Restart Windows to finish installing the Microsoft runtime, then run Setup again.';
  end else if not RuntimeReady() then
    Result := Format('The Microsoft Visual C++ runtime is unavailable (installer exit code %d).', [ExitCode]);
end;
