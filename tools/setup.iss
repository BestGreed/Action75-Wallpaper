#ifndef PayloadDir
  #error Use tools/package.ps1 to stage the release payload.
#endif
#ifdef QaId
  #define AppName "Action75 Wallpaper QA " + QaId
  #define InstallId "Action75WallpaperQA-" + QaId
  #define Registration "Action75WallpaperQA-" + QaId + "_is1"
  #define InstallFolder "ACTION75-QA-" + QaId
  #define InstallerName "Action75-Wallpaper-QA-Setup"
#else
  #define AppName "Action75 Wallpaper"
  #define InstallId "{{81CCDD80-9A70-4694-94EB-AC6D96651B70}"
  #define Registration "{81CCDD80-9A70-4694-94EB-AC6D96651B70}_is1"
  #define InstallFolder "ACTION75"
  #define InstallerName "Action75-Wallpaper-Setup"
#endif
[Setup]
AppId={#InstallId}
AppName={#AppName}
AppVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\{#InstallFolder}
DefaultGroupName={#AppName}
UsePreviousGroup=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
DisableDirPage=no
DisableProgramGroupPage=yes
OutputDir=..\release
OutputBaseFilename={#InstallerName}-{#AppVersion}-x64
SetupIconFile={#PayloadDir}\assets\app.ico
VersionInfoDescription={#AppName}
VersionInfoProductName={#AppName}
VersionInfoVersion={#AppVersion}
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\dist\Action75Manager\Action75Manager.exe
Compression=lzma2/fast
SolidCompression=yes
WizardStyle=modern
#ifdef QaId
CloseApplications=no
#else
CloseApplications=yes
#endif
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked

#ifndef QaId
[InstallDelete]
Type: files; Name: "{userprograms}\ACTION75\ACTION75.lnk"; Check: HasPreviousInstall
Type: dirifempty; Name: "{userprograms}\ACTION75"; Check: HasPreviousInstall
Type: files; Name: "{autodesktop}\ACTION75.lnk"; Check: HasPreviousInstall
#endif

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\dist\Action75Manager\Action75Manager.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\dist\Action75Manager\Action75Manager.exe"; WorkingDir: "{app}"; Tasks: desktopicon

#ifndef QaId
[Run]
Filename: "{app}\dist\Action75Manager\Action75Manager.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

#endif

[Code]
#ifndef QaId
function HasPreviousInstall: Boolean;
begin
  Result := RegKeyExists(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{81CCDD80-9A70-4694-94EB-AC6D96651B70}_is1');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', 'ACTION75');
end;
#endif

var
  UpdateParent: Integer;
  UpdateToken, UpdateDir, UpdateManager: String;
  ResumePanel, Installed: Boolean;

function OpenProcess(Access: Cardinal; Inherit: Boolean; Pid: Cardinal): Integer;
  external 'OpenProcess@kernel32.dll stdcall';
function QueryFullProcessImageName(Process: Integer; Flags: Cardinal; Buffer: String; var Size: Cardinal): Boolean;
  external 'QueryFullProcessImageNameW@kernel32.dll stdcall';
function OpenEvent(Access: Cardinal; Inherit: Boolean; Name: String): Integer;
  external 'OpenEventW@kernel32.dll stdcall';
function SetEvent(Event: Integer): Boolean;
  external 'SetEvent@kernel32.dll stdcall';
function WaitForSingleObject(Handle: Integer; Milliseconds: Cardinal): Cardinal;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(Handle: Integer): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

function SameDir(A, B: String): Boolean;
begin
  Result := CompareText(RemoveBackslashUnlessRoot(ExpandFileName(A)),
                        RemoveBackslashUnlessRoot(ExpandFileName(B))) = 0;
end;

function InitializeSetup: Boolean;
var
  Pid, I: Integer;
  Count, OldMS, OldLS, NewMS, NewLS: Cardinal;
  ImagePath: String;
begin
  Result := True;
  if ExpandConstant('{param:A75UPDATE|}') = '' then exit;
  Result := False;
  UpdateToken := ExpandConstant('{param:A75READY|}');
  if Length(UpdateToken) <> 32 then exit;
  for I := 1 to Length(UpdateToken) do
    if Pos(UpdateToken[I], '0123456789abcdef') = 0 then exit;
  if not RegQueryStringValue(HKCU64,
     'Software\Microsoft\Windows\CurrentVersion\Uninstall\{#Registration}',
     'InstallLocation', UpdateDir) then exit;
  if not SameDir(UpdateDir, ExpandConstant('{param:DIR|}')) then exit;
  UpdateManager := AddBackslash(UpdateDir) + 'dist\Action75Manager\Action75Manager.exe';
  if not GetVersionNumbers(UpdateManager, OldMS, OldLS) then exit;
  if not GetVersionNumbers(ExpandConstant('{srcexe}'), NewMS, NewLS) then exit;
  if (NewMS < OldMS) or ((NewMS = OldMS) and (NewLS <= OldLS)) then exit;
  Pid := StrToIntDef(ExpandConstant('{param:A75PARENT|0}'), 0);
  if Pid <= 0 then exit;
  UpdateParent := OpenProcess($101000, False, Pid);
  if UpdateParent = 0 then exit;
  Count := 32768;
  SetLength(ImagePath, Count);
  if not QueryFullProcessImageName(UpdateParent, 0, ImagePath, Count) then exit;
  SetLength(ImagePath, Count);
  Result := SameDir(UpdateManager, ImagePath);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Event, Code: Integer;
begin
  Result := '';
  if UpdateToken = '' then exit;
  if not SameDir(UpdateDir, WizardDirValue) then begin
    Result := 'The selected directory differs from the registered installation.';
    exit;
  end;
  Event := OpenEvent(2, False, 'Local\Action75Update-' + UpdateToken);
  if Event = 0 then begin
    Result := 'The controller update session has expired.';
    exit;
  end;
  try
    if not SetEvent(Event) then begin
      Result := 'Cannot acknowledge the controller update.';
      exit;
    end;
  finally
    CloseHandle(Event);
  end;
  if WaitForSingleObject(UpdateParent, 45000) <> 0 then begin
    Result := 'The controller did not exit. Installation was cancelled.';
    exit;
  end;
  ResumePanel := True;
  if not Exec(UpdateManager, '--update-stop ' + UpdateToken, UpdateDir, SW_HIDE,
              ewWaitUntilTerminated, Code) or (Code <> 0) then
    Result := 'The installed player could not be stopped safely.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then Installed := True;
end;

procedure DeinitializeSetup;
var
  Code: Integer;
  Args: String;
begin
  if UpdateParent <> 0 then CloseHandle(UpdateParent);
  if ResumePanel then begin
    Args := '--update-restore ' + UpdateToken;
    if not Installed then Args := Args + ' --update-failed';
    if not Exec(UpdateManager, Args, UpdateDir, SW_HIDE,
                ewWaitUntilTerminated, Code) or (Code <> 0) then begin
      Log('The update could not restore the original session.');
      SuppressibleMsgBox('Could not restore the previous wallpaper session. Open the controller to retry.',
                         mbError, MB_OK, IDOK);
    end;
  end;
end;
