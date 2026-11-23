; SPDX-License-Identifier: GPL-3.0-or-later
;
; TD_Custom_FFT - Windows installer for the FFT Custom CHOP (Inno Setup 6.5 or later; built with 6.7.3).
;
; Installs per user - no administrator rights, no UAC prompt:
;   {userdocs}\Derivative\Plugins\FFT\      component "core": FFT.dll, the vendored FFTW3 DLL, the license files
;                                           component "onemkl": Intel oneMKL, DOWNLOADED from nuget.org at install
;                                             time (pinned package + SHA-256, never redistributed by this project)
;   {userdocs}\TD_Custom_FFT\Examples\      component "examples": the example project(s)
;
; Refuses or warns before anything is copied:
;   - a CPU without AVX2 (the plugin needs AVX2 + FMA; without them the node only ever shows an error);
;   - TouchDesigner running (it keeps the plugin DLLs mapped, so they could not be replaced);
;   - a Visual C++ runtime older than the compiler FFT.dll was built with, or missing (offers to download and
;     run Microsoft's redistributable, which asks for elevation itself).
;
; BUILD: python installer\build_release.py - it stages the files into installer\stage\ and writes
; stage\release.iss (version, the oneMKL pin from installer\onemkl.json, the VC++ runtime floor read from the
; build), then runs ISCC. Do not compile this file against a hand-made stage: release.iss is what keeps the
; installer in step with the build it packages.
;
; Lineage: EssentiaTD's installer (github.com/DarienBrito/EssentiaTD, installer/windows/EssentiaTD.iss) - the
; {userdocs} target, PrivilegesRequired=lowest, the stale-DLL wipe and the TouchDesigner-running loop with its
; silent-mode rule are taken from it. The components, the oneMKL download, the CPU and VC++ checks, the upgrade
; that keeps an up-to-date oneMKL instead of downloading it again, and the uninstaller kept out of TouchDesigner's
; plugin folder are this project's additions. Design notes: docs/INSTALLER.md.

#ifndef StageDir
  #define StageDir "stage"
#endif
#ifndef OutputDir
  #define OutputDir "out"
#endif
#ifndef RepoDir
  #define RepoDir ".."
#endif
#include AddBackslash(StageDir) + "release.iss"

#define AppName "TD Custom FFT"
#define RepoUrl "https://github.com/NairoDorian/TD_Custom_FFT"
; Microsoft's evergreen link to the current Visual C++ v14 redistributable (x64).
#define VcRedistUrl "https://aka.ms/vc14/vc_redist.x64.exe"

[Setup]
; Never change AppId: it is how an upgrade finds the previous installation (and its uninstaller).
AppId={{6F2B8E1D-4C3A-4F57-9E0B-2D7A1C9F5E43}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=NairoDorian
AppPublisherURL={#RepoUrl}
AppSupportURL={#RepoUrl}/issues
AppUpdatesURL={#RepoUrl}/releases
AppComments=FFT Custom CHOP for TouchDesigner
VersionInfoVersion={#AppVersion}
VersionInfoDescription={#AppName} {#AppVersion} Setup
; The folder PluginBuilder's Install Plugin step uses too, so an installed machine and a dev machine look the
; same - and an upgrade over a PluginBuilder install replaces it instead of registering the operator twice.
DefaultDirName={userdocs}\Derivative\Plugins\FFT
DisableProgramGroupPage=yes
LicenseFile={#RepoDir}\LICENSE
PrivilegesRequired=lowest
OutputDir={#OutputDir}
; Unversioned, so https://github.com/NairoDorian/TD_Custom_FFT/releases/latest/download/TD_Custom_FFT-Setup.exe
; is a permanent link. The version is in the file's properties and the wizard title.
OutputBaseFilename=TD_Custom_FFT-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; .nupkg is a zip; "full" is the extraction method that reads zip archives.
ArchiveExtraction=full
; The uninstaller lives beside the plugin's own per-user cache, not in TouchDesigner's plugin folder, which then
; holds plugin files only.
UninstallFilesDir={localappdata}\TD_Custom_FFT\Uninstall
UninstallDisplayName={#AppName} {#AppVersion}
UninstallDisplayIcon={uninstallexe}
; TouchDesigner is handled by PrepareToInstall below (Retry/Cancel, abort when silent); the Restart Manager page
; would offer to close it for the user, which can lose unsaved work.
CloseApplications=no
SetupLogging=yes

[Messages]
WelcomeLabel2=This will install [name/ver]: the FFT Custom CHOP, a real-time audio spectrum operator for TouchDesigner.%n%nRequirements: Windows 10 or 11 (64-bit), a CPU with AVX2 and FMA (most CPUs since 2013), TouchDesigner 2025.30000 or later.%n%nClose TouchDesigner before you continue.
FinishedLabel=Setup has installed [name] on your computer.%n%nStart TouchDesigner, press Tab and create CHOP > FFT Custom. If TouchDesigner asks whether to load the plugin, allow it.

[Types]
Name: "standard"; Description: "Standard: FFT CHOP (FFTW3 backend) and the example project"
Name: "full"; Description: "Full: also the Intel oneMKL backend (166 MB download from nuget.org)"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "core"; Description: "FFT Custom CHOP {#AppVersion} (FFT.dll + FFTW 3.3.11 AVX2)"; Types: standard full custom; Flags: fixed
Name: "onemkl"; Description: "Intel oneMKL {#OneMklVersion} backend: optional, a faster FFT on the analysis thread. Downloaded from nuget.org (166 MB), Intel Simplified Software License"; Types: full; ExtraDiskSpaceRequired: {#OneMklInstalledSize}
Name: "examples"; Description: "Example project: Circular Audio Spectrum (Documents\TD_Custom_FFT\Examples)"; Types: standard full

[Files]
; The stage is curated by build_release.py (FFT.dll + exactly the runtime files the build's manifest names), so a
; glob here is safe. Never point this at a dev __Plugins__ folder: it has held .old files and removed runtimes.
Source: "{#StageDir}\FFT\*"; DestDir: "{app}"; Components: core; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\Examples\*"; DestDir: "{userdocs}\TD_Custom_FFT\Examples"; Components: examples; Flags: ignoreversion recursesubdirs createallsubdirs
; oneMKL: downloaded and extracted into {tmp}\onemkl by NextButtonClick(wpReady) below. external = copied from
; there at install time and recorded for uninstall like any other file. Skipped (Check) when this run did not
; download it - the component is not selected, or the set already installed is the pinned one and is kept.
#define I 0
#sub OneMklDllEntry
Source: "{tmp}\onemkl\{#OneMklDllDir}\{#OneMklDlls[I]}"; DestDir: "{app}"; Components: onemkl; Check: OneMklFetched; Flags: external ignoreversion
#endsub
#for {I = 0; I < DimOf(OneMklDlls); I++} OneMklDllEntry
#sub OneMklLicenseEntry
Source: "{tmp}\onemkl\{#OneMklLicSrc[I]}"; DestDir: "{app}\oneMKL-licenses"; DestName: "{#OneMklLicDst[I]}"; Components: onemkl; Check: OneMklFetched; Flags: external ignoreversion
#endsub
#for {I = 0; I < DimOf(OneMklLicSrc); I++} OneMklLicenseEntry

[InstallDelete]
; Upgrades never leave orphans: every file of ours is copied again, and a stray plugin DLL would make
; TouchDesigner register the operator twice.
Type: files; Name: "{app}\FFT.dll"
Type: files; Name: "{app}\*.dll.old*"
Type: files; Name: "{app}\libfftw3f-*.dll"
; Files an older manual install could have left beside FFT.dll that the plugin never loads. libiomp5md.dll in
; particular must not be there: TouchDesigner loads its own Intel OpenMP, and two in one process abort.
Type: files; Name: "{app}\libimalloc.dll"
Type: files; Name: "{app}\libiomp5md.dll"
Type: files; Name: "{app}\mkl_intel_thread.3.dll"
Type: files; Name: "{app}\mkl_tbb_thread.3.dll"
; oneMKL: removed when its component is not selected, replaced when this run downloaded a new set, kept otherwise.
Type: files; Name: "{app}\mkl_*.dll"; Check: OneMklRemoveOld
Type: filesandordirs; Name: "{app}\oneMKL-licenses"; Check: OneMklRemoveOld
; Loose copies at the root of the Plugins folder, from manual installs that predate this installer.
Type: files; Name: "{userdocs}\Derivative\Plugins\FFT.dll"
Type: files; Name: "{userdocs}\Derivative\Plugins\libfftw3f-3.3.11-avx2.dll"

[UninstallDelete]
; Files the uninstall log may not list: PluginBuilder's rename-in-place leftovers, a oneMKL set kept from an
; earlier run or copied by install.ps1, and the marker written below.
Type: files; Name: "{app}\*.dll.old*"
Type: files; Name: "{app}\mkl_*.dll"
Type: filesandordirs; Name: "{app}\oneMKL-licenses"
Type: dirifempty; Name: "{app}"
Type: dirifempty; Name: "{userdocs}\TD_Custom_FFT\Examples"
Type: dirifempty; Name: "{userdocs}\TD_Custom_FFT"

[Run]
Filename: "{userdocs}\TD_Custom_FFT\Examples\{#ExampleMain}"; Description: "Open the example project in TouchDesigner"; Flags: postinstall shellexec skipifsilent nowait unchecked; Components: examples
Filename: "{app}"; Description: "Open the plugin folder"; Flags: postinstall shellexec skipifsilent nowait unchecked

[Code]
const
  PF_AVX2_INSTRUCTIONS_AVAILABLE = 40;
  OneMklZip = 'onemkl.zip';
  VcRedistExe = 'vc_redist.x64.exe';

// Windows' own answer for AVX2. Windows exposes no FMA flag, but every x86 CPU with AVX2 also has FMA3; the
// plugin checks both itself (cpuSupportsAVX2 in DSPModules.h) and reports a VM that masks FMA as an error.
function IsProcessorFeaturePresent(Feature: Cardinal): BOOL;
  external 'IsProcessorFeaturePresent@kernel32.dll stdcall';

var
  DownloadPage: TDownloadWizardPage;
  ExtractPage: TExtractionWizardPage;
  OneMklWasFetched: Boolean;   // this run downloaded and extracted the pinned oneMKL into {tmp}\onemkl

// A file's version resource as "a.b.c.d", or '' when it has none (or does not exist).
function FileVersionOf(const FileName: String): String;
begin
  if not GetVersionNumbersString(FileName, Result) then
    Result := '';
end;

// ---- oneMKL -----------------------------------------------------------------------------------------------

function OneMklDlls: TStringList;
begin
  Result := TStringList.Create;
  Result.CommaText := '{#OneMklDllCsv}';
end;

// The pinned set is already in {app}: all DLLs present, and either the marker this installer (or install.ps1)
// writes names the pinned package, or mkl_rt carries the pinned file version (a set copied by hand). Then an
// upgrade keeps it instead of downloading 166 MB again.
function OneMklCurrent: Boolean;
var
  Dlls: TStringList;
  Marker: AnsiString;
  I: Integer;
begin
  Result := True;
  Dlls := OneMklDlls;
  try
    for I := 0 to Dlls.Count - 1 do
      if not FileExists(ExpandConstant('{app}\') + Dlls[I]) then
        Result := False;
  finally
    Dlls.Free;
  end;
  if Result then
    Result := (LoadStringFromFile(ExpandConstant('{app}\oneMKL-licenses\{#OneMklMarkerFile}'), Marker)
               and (Trim(String(Marker)) = '{#OneMklMarker}'))
              or (FileVersionOf(ExpandConstant('{app}\mkl_rt.3.dll')) = '{#OneMklFileVersion}');
end;

function OneMklFetched: Boolean;
begin
  Result := OneMklWasFetched;
end;

function OneMklRemoveOld: Boolean;
begin
  Result := (not WizardIsComponentSelected('onemkl')) or OneMklWasFetched;
end;

// ---- Visual C++ runtime -----------------------------------------------------------------------------------

// FFT.dll is built /MD and loads the system's Visual C++ runtime, which must be at least as new as the compiler
// that built it (Microsoft's rule; ignoring it is how binaries built with MSVC 14.40+ crash in std::mutex on an
// older msvcp140.dll). VCRUNTIME140_1 and MSVCP140_ATOMIC_WAIT only exist in newer redistributables at all.
function VcRuntimeOk: Boolean;
var
  MS, LS: Cardinal;
begin
  Result := FileExists(ExpandConstant('{sys}\vcruntime140_1.dll'))
            and FileExists(ExpandConstant('{sys}\msvcp140_atomic_wait.dll'))
            and GetVersionNumbers(ExpandConstant('{sys}\msvcp140.dll'), MS, LS)
            and (MS >= ({#MinVcMajor} * 65536 + {#MinVcMinor}));
end;

function VcRuntimeVersion: String;
begin
  Result := FileVersionOf(ExpandConstant('{sys}\msvcp140.dll'));
  if Result = '' then Result := 'not installed';
end;

// ---- TouchDesigner running --------------------------------------------------------------------------------

// A running TouchDesigner keeps the plugin DLLs mapped: deletes and overwrites fail, and it would keep using the
// old module until restarted. (EssentiaTD's check, unchanged.)
function TouchDesignerRunning: Boolean;
var
  Locator, WMI, Results: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    WMI := Locator.ConnectServer('', 'root\CIMV2');
    Results := WMI.ExecQuery('SELECT Name FROM Win32_Process WHERE Name LIKE ''TouchDesigner%''');
    Result := (Results.Count > 0);
  except
    // WMI unavailable: do not block the install.
    Result := False;
  end;
end;

// ---- Wizard -----------------------------------------------------------------------------------------------

function InitializeSetup: Boolean;
begin
  Result := True;
  if not IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) then
  begin
    SuppressibleMsgBox('This computer''s processor does not support AVX2, which the FFT Custom CHOP requires '
      + '(it is built for AVX2 + FMA: Intel since 2013, AMD since 2015). The operator would only ever show an '
      + 'error here, so Setup will exit.', mbCriticalError, MB_OK, IDOK);
    Result := False;
  end;
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing), 'Downloading the optional components...', nil);
  DownloadPage.ShowBaseNameInsteadOfUrl := True;
  ExtractPage := CreateExtractionPage(SetupMessage(msgWizardPreparing), 'Unpacking Intel oneMKL...', nil);
end;

// The Ready page says what will happen to oneMKL and the VC++ runtime, so a large download or the removal of a
// set copied by hand is never a surprise.
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
var
  Notes: String;
begin
  Result := MemoDirInfo + NewLine + NewLine + MemoTypeInfo + NewLine + NewLine + MemoComponentsInfo;
  Notes := '';
  if WizardIsComponentSelected('onemkl') then
  begin
    if OneMklCurrent then
      Notes := Notes + Space + 'Intel oneMKL {#OneMklVersion} is already installed: kept, nothing to download.' + NewLine
    else
      Notes := Notes + Space + 'Intel oneMKL {#OneMklVersion} will be downloaded from nuget.org (166 MB) and checked against its pinned SHA-256.' + NewLine;
  end
  else if FileExists(ExpandConstant('{app}\mkl_rt.3.dll')) then
    Notes := Notes + Space + 'Intel oneMKL found in the plugin folder will be REMOVED (the oneMKL component is not selected).' + NewLine;
  if not VcRuntimeOk then
    Notes := Notes + Space + 'The Visual C++ runtime is older than {#MinVcMajor}.{#MinVcMinor} (found: ' + VcRuntimeVersion
      + '). Setup will offer to install the current one from Microsoft.' + NewLine;
  if Notes <> '' then
    Result := Result + NewLine + NewLine + 'Notes:' + NewLine + Notes;
end;

// Runs the downloads before PrepareToInstall: the optional oneMKL package (when selected and not already current)
// and, when the user agrees, Microsoft's VC++ redistributable. Called on silent installs too (Setup simulates the
// Next clicks); there every question takes its default answer.
function NextButtonClick(CurPageID: Integer): Boolean;
var
  WantMkl, WantVc: Boolean;
  Dlls: TStringList;
  I, Code: Integer;
  Missing: String;
begin
  Result := True;
  if CurPageID <> wpReady then Exit;

  OneMklWasFetched := False;
  WantMkl := WizardIsComponentSelected('onemkl') and not OneMklCurrent;
  WantVc := False;
  if not VcRuntimeOk then
    WantVc := SuppressibleMsgBox('The FFT Custom CHOP needs the Microsoft Visual C++ runtime {#MinVcMajor}.{#MinVcMinor} or newer; '
      + 'this computer has: ' + VcRuntimeVersion + '. With an older one TouchDesigner may refuse to load the plugin or crash.'
      + #13#10#13#10 + 'Download and install the current Visual C++ Redistributable from Microsoft now? '
      + '(about 25 MB; Windows will ask for administrator rights)', mbConfirmation, MB_YESNO, IDNO) = IDYES;
  if not (WantMkl or WantVc) then Exit;

  DownloadPage.Clear;
  if WantMkl then DownloadPage.Add('{#OneMklUrl}', OneMklZip, '{#OneMklSha256}');
  if WantVc then DownloadPage.Add('{#VcRedistUrl}', VcRedistExe, '');
  DownloadPage.Show;
  try
    try
      DownloadPage.Download;
    except
      if DownloadPage.AbortedByUser then
        Log('Download aborted by the user.')
      else
        Log('Download failed: ' + DownloadPage.LastBaseNameOrUrl + ': ' + GetExceptionMessage);
      if WantMkl and not FileExists(ExpandConstant('{tmp}\') + OneMklZip) then
      begin
        // The plugin runs without oneMKL (FFTW3 is the default backend), so offer to go on without it.
        if SuppressibleMsgBox('Intel oneMKL could not be downloaded (' + GetExceptionMessage + ').'
          + #13#10#13#10 + 'Continue without it? The FFT Custom CHOP works on its FFTW3 backend; you can run Setup '
          + 'again later to add oneMKL.', mbError, MB_YESNO, IDYES) = IDNO then
        begin
          Result := False;
          Exit;
        end;
        WantMkl := False;
      end;
      if WantVc and not FileExists(ExpandConstant('{tmp}\') + VcRedistExe) then
        WantVc := False;
    end;
  finally
    DownloadPage.Hide;
  end;

  if WantVc then
  begin
    // vc_redist asks for elevation itself (its manifest requires it); ShellExec lets Windows show that prompt.
    if not ShellExec('', ExpandConstant('{tmp}\') + VcRedistExe, '/install /passive /norestart', '',
                     SW_SHOWNORMAL, ewWaitUntilTerminated, Code) then
      Log('vc_redist could not be started: ' + SysErrorMessage(Code))
    else
      Log('vc_redist exit code ' + IntToStr(Code) + ' (0 = installed, 1638 = a newer one is already present, 3010 = reboot needed)');
    if not VcRuntimeOk then
      SuppressibleMsgBox('The Visual C++ runtime is still older than {#MinVcMajor}.{#MinVcMinor} (found: ' + VcRuntimeVersion
        + '). Setup continues; install "Microsoft Visual C++ Redistributable (x64)" from Microsoft if TouchDesigner '
        + 'cannot load the plugin.', mbInformation, MB_OK, IDOK);
  end;

  if WantMkl then
  begin
    ExtractPage.Clear;
    ExtractPage.Add(ExpandConstant('{tmp}\') + OneMklZip, ExpandConstant('{tmp}\onemkl'), True);
    ExtractPage.Show;
    try
      try
        ExtractPage.Extract;
        DeleteFile(ExpandConstant('{tmp}\') + OneMklZip);
        Missing := '';
        Dlls := OneMklDlls;
        try
          for I := 0 to Dlls.Count - 1 do
            if not FileExists(ExpandConstant('{tmp}\onemkl\{#OneMklDllDir}\') + Dlls[I]) then
              Missing := Missing + ' ' + Dlls[I];
        finally
          Dlls.Free;
        end;
        if Missing <> '' then
          RaiseException('the package does not contain' + Missing);
        OneMklWasFetched := True;
      except
        if SuppressibleMsgBox('Intel oneMKL could not be unpacked (' + GetExceptionMessage + ').'
          + #13#10#13#10 + 'Continue without it?', mbError, MB_YESNO, IDYES) = IDNO then
          Result := False;
      end;
    finally
      ExtractPage.Hide;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  // Silent mode: abort at once. /SUPPRESSMSGBOXES answers MB_RETRYCANCEL with its default (Retry), which would
  // spin this loop forever. (EssentiaTD's rule.)
  if WizardSilent then
  begin
    if TouchDesignerRunning then
      Result := 'TouchDesigner is running. Close it and run Setup again.';
    Exit;
  end;
  while TouchDesignerRunning do
  begin
    if MsgBox('TouchDesigner is running. It keeps the plugin files open, so Setup cannot update them.'
              + #13#10#13#10 + 'Close TouchDesigner (save your project first), then press Retry.',
              mbError, MB_RETRYCANCEL) = IDCANCEL then
    begin
      Result := 'Installation cancelled because TouchDesigner is running.';
      Exit;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  // Marks the installed oneMKL as the pinned package, so the next upgrade can keep it (OneMklCurrent).
  if (CurStep = ssPostInstall) and OneMklWasFetched then
    SaveStringToFile(ExpandConstant('{app}\oneMKL-licenses\{#OneMklMarkerFile}'), '{#OneMklMarker}' + #13#10, False);
end;

// ---- Uninstall --------------------------------------------------------------------------------------------

function InitializeUninstall: Boolean;
begin
  Result := True;
  while TouchDesignerRunning do
  begin
    if UninstallSilent then
    begin
      Log('TouchDesigner is running; uninstall aborted.');
      Result := False;
      Exit;
    end;
    if MsgBox('TouchDesigner is running and keeps the plugin files open.' + #13#10#13#10
              + 'Close TouchDesigner, then press Retry.', mbError, MB_RETRYCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

// The FFTW planning cache (wisdom: fftwf_wisdom.txt, plus a per-backend fftwf_wisdom_<id>.txt) is per machine and
// only a few kilobytes, but it is the plugin's, so the user decides whether it goes. Kept on silent uninstalls.
// Only the wisdom files are deleted: the uninstaller's own folder sits beside them and is still in use here.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Cache: String;
begin
  if CurUninstallStep <> usPostUninstall then Exit;
  Cache := ExpandConstant('{localappdata}\TD_Custom_FFT');
  if FileExists(Cache + '\fftwf_wisdom.txt') and not UninstallSilent then
    if MsgBox('Also delete the FFT planning cache (' + Cache + '\fftwf_wisdom*.txt)?' + #13#10#13#10
              + 'It only makes the first use of each FFT size faster; it is rebuilt automatically.',
              mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
      DelTree(Cache + '\fftwf_wisdom*.txt', False, True, False);
end;
