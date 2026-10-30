; Setup.exe for Streets of SimCity with the fix. Built by tools/package.py, which passes:
;   /DAppVersion=v1.0.0  /DStage=<folder holding the release files>  /DOutDir=<output folder>
; The installer adds the fix, then gets the game from its CD image: downloaded from the
; Internet Archive (checked against the SHA-256 of the image the fix was made with), or a copy
; the player already has (the .iso, or the oldgamesdownload.com zip). install_game.ps1 does the
; copying and logs to logs\install_game.log. No admin rights needed: it installs per user.

#ifndef AppVersion
  #define AppVersion "dev"
#endif
#ifndef Stage
  #define Stage "..\out\stage"
#endif
#ifndef OutDir
  #define OutDir "..\out"
#endif

#define IsoUrl    "https://archive.org/download/Streets_of_SimCity_Maxis_1997/Streets.iso"
#define IsoSha256 "2da3e9d8dc5a995c3029c369227ce25e7e34268fa2289fc4151f846ed8f494d2"
#define IsoMB     "508"

[Setup]
AppId={{7B1C9E52-3D6A-4F0B-9C21-5E8A4D7F2B90}
AppName=Streets of SimCity
AppVersion={#AppVersion}
AppVerName=Streets of SimCity (fix {#AppVersion})
AppPublisher=github.com/jasonwarta/sosc
AppPublisherURL=https://github.com/jasonwarta/sosc
DefaultDirName={autopf}\Streets of SimCity
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir={#OutDir}
OutputBaseFilename=StreetsOfSimCity-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\Streets.exe
UninstallDisplayName=Streets of SimCity
SetupLogging=yes

[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "install_game.ps1"; Flags: dontcopy

[Icons]
Name: "{autoprograms}\Streets of SimCity"; Filename: "{app}\Streets.exe"; Parameters: "-o2"; WorkingDir: "{app}"
Name: "{autoprograms}\Streets of SimCity settings"; Filename: "{app}\streets_fix.ini"
Name: "{autoprograms}\Streets of SimCity logs"; Filename: "{app}\logs"
Name: "{autodesktop}\Streets of SimCity"; Filename: "{app}\Streets.exe"; Parameters: "-o2"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\Streets.exe"; Parameters: "-o2"; WorkingDir: "{app}"; Description: "Play Streets of SimCity now"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
; The game's files were copied in by install_game.ps1, so the uninstaller doesn't know them.
Type: filesandordirs; Name: "{app}\BMP"
Type: filesandordirs; Name: "{app}\Cities"
Type: filesandordirs; Name: "{app}\FONTS"
Type: filesandordirs; Name: "{app}\Games"
Type: filesandordirs; Name: "{app}\GEO"
Type: filesandordirs; Name: "{app}\HELP"
Type: filesandordirs; Name: "{app}\scenarios"
Type: filesandordirs; Name: "{app}\shared"
Type: filesandordirs; Name: "{app}\SMK"
Type: filesandordirs; Name: "{app}\SOUND"
Type: filesandordirs; Name: "{app}\Weapons"
Type: filesandordirs; Name: "{app}\X"
Type: filesandordirs; Name: "{app}\logs"
Type: filesandordirs; Name: "{app}\screenshots"
Type: files; Name: "{app}\*.exe"
Type: files; Name: "{app}\*.dll"
Type: files; Name: "{app}\*.DLL"
Type: files; Name: "{app}\*.txt"
Type: files; Name: "{app}\*.wri"
Type: files; Name: "{app}\*.GID"
Type: files; Name: "{app}\*.HLP"
Type: files; Name: "{app}\*.INF"
Type: files; Name: "{app}\*.SC2"
Type: files; Name: "{app}\*.DB"
Type: files; Name: "{app}\*.cfg"
Type: dirifempty; Name: "{app}"

[Code]
var
  SourcePage: TInputOptionWizardPage;
  FilePage: TInputFileWizardPage;
  DownloadPage: TDownloadWizardPage;

function GameInstalled: Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\Streets.exe')) and DirExists(ExpandConstant('{app}\GEO'));
end;

procedure InitializeWizard;
begin
  SourcePage := CreateInputOptionPage(wpSelectDir, 'Game files',
    'Where should Setup get Streets of SimCity from?',
    'The fix needs the original game. It is abandonware and not included here.', True, False);
  SourcePage.Add('Download it from the Internet Archive ({#IsoMB} MB, the original CD)');
  SourcePage.Add('Use a copy I already have (the .iso, or the zip from oldgamesdownload.com)');
  SourcePage.SelectedValueIndex := 0;

  FilePage := CreateInputFilePage(SourcePage.ID, 'Game files', 'Which file holds the game?',
    'Pick the Streets of SimCity CD image (.iso) or the Streets_of_SimCity_Win_ISO_EN.zip download.');
  FilePage.Add('CD image or zip:', 'Streets of SimCity CD (*.iso;*.zip)|*.iso;*.zip|All files|*.*', '.iso');

  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing), 'Downloading the game...', nil);
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  // Updating an install that already has the game: only the fix changes.
  Result := ((PageID = SourcePage.ID) and GameInstalled) or
            ((PageID = FilePage.ID) and (GameInstalled or (SourcePage.SelectedValueIndex <> 1)));
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = FilePage.ID) and not FileExists(FilePage.Values[0]) then begin
    MsgBox('Pick the .iso or .zip file holding the game.', mbError, MB_OK);
    Result := False;
  end else if (CurPageID = wpReady) and not GameInstalled and (SourcePage.SelectedValueIndex = 0) then begin
    DownloadPage.Clear;
    DownloadPage.Add('{#IsoUrl}', 'Streets.iso', '{#IsoSha256}');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
      except
        if DownloadPage.AbortedByUser then
          Log('download aborted')
        else
          SuppressibleMsgBox('The game download failed: ' + GetExceptionMessage + #13#10#13#10 +
            'You can download Streets_of_SimCity_Win_ISO_EN.zip from oldgamesdownload.com yourself and ' +
            'choose "Use a copy I already have".', mbCriticalError, MB_OK, IDOK);
        Result := False;
      end;
    finally
      DownloadPage.Hide;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Source, Params: String;
  Code: Integer;
begin
  if (CurStep <> ssPostInstall) or GameInstalled then Exit;
  if SourcePage.SelectedValueIndex = 0 then
    Source := ExpandConstant('{tmp}\Streets.iso')
  else
    Source := FilePage.Values[0];
  ExtractTemporaryFile('install_game.ps1');
  WizardForm.StatusLabel.Caption := 'Copying the game from its CD image...';
  Params := '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{tmp}\install_game.ps1') +
            '" -Source "' + Source + '" -Dest "' + ExpandConstant('{app}') + '"';
  if not Exec('powershell.exe', Params, '', SW_HIDE, ewWaitUntilTerminated, Code) or (Code <> 0) then
    MsgBox('Copying the game failed (code ' + IntToStr(Code) + '). Details are in ' +
           ExpandConstant('{app}\logs\install_game.log') + '.' + #13#10#13#10 +
           'You can still mount the CD image yourself and run "Copy game from CD.cmd" in that folder.',
           mbError, MB_OK);
end;
