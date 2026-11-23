# Installing TD_Custom_FFT: how the installers and the release work

**Current as of:** v2.14.0 (2026-11-23). For the user-facing steps, see [Install](../README.md#install) in the
README; this file is for whoever maintains the installers or cuts a release.

The files live in [`installer/`](../installer):

| File | What it is |
|---|---|
| `TD_Custom_FFT.iss` | The Inno Setup 6.5+ script for `TD_Custom_FFT-Setup.exe` |
| `install.ps1` | The script installer (Windows PowerShell 5.1 and PowerShell 7): `irm … \| iex`, `Install.cmd`, or `-File` |
| `Install.cmd` | Double-click wrapper for `install.ps1` inside the release zip |
| `README-zip.txt` | The `README.txt` of the release zip |
| `onemkl.json` | **The** pin of the optional Intel oneMKL runtime (package, URL, size, SHA-256, file list) |
| `build_release.py` | Stages a finished build and writes every release asset into `installer/dist/` |

---

## 1. What a release contains

| Asset | For | Contents |
|---|---|---|
| `TD_Custom_FFT-Setup.exe` | Most users | Wizard; components *core* (fixed), *oneMKL* (optional download), *examples* |
| `TD_Custom_FFT-Windows.zip` | Manual or offline installs | `FFT/` (the plugin folder), `Examples/`, `install.ps1`, `Install.cmd`, `README.txt`, `manifest.json` |
| `install.ps1` | Scripted installs | The script stamped with the release tag and the zip's SHA-256 |
| `fftw-3.3.11.tar.gz` + `fftw-3.3.11-cmake-version.patch` | GPL compliance | FFTW's corresponding source (promised by `3rdParty/fftw3/SOURCE.md`) |
| `SHA256SUMS.txt` | Everyone | The hash of every asset above |

Asset names are unversioned, so `https://github.com/NairoDorian/TD_Custom_FFT/releases/latest/download/<name>`
is a permanent link. The version is in the Setup file's properties, the wizard title and `manifest.json`.

## 2. What lands where

Per user, no administrator rights, no UAC prompt (the VC++ redistributable is the one exception, and only when
the user agrees to install it).

| Path | Files | Component |
|---|---|---|
| `<Documents>\Derivative\Plugins\FFT\` | `FFT.dll`, `libfftw3f-3.3.11-avx2.dll`, `FFT-LICENSE.txt`, `FFT-NOTICE.md`, `FFTW3-COPYING.txt`, `THIRD_PARTY_NOTICES.md` | core |
| same folder | the 14 `mkl_*.3.dll` (456 MiB) | oneMKL |
| `…\FFT\oneMKL-licenses\` | `license.txt`, `third-party-programs.txt` (Intel's), `nuget-package.txt` (marker, below) | oneMKL |
| `<Documents>\TD_Custom_FFT\Examples\` | `Circular_Audio_Spectrum.toe` | examples |
| `%LOCALAPPDATA%\TD_Custom_FFT\Uninstall\` | `unins000.exe/.dat` (Setup.exe only) | — |
| `%LOCALAPPDATA%\TD_Custom_FFT\` | `fftwf_wisdom.txt`, written by the plugin itself on first use | — |

- `<Documents>` is the real Documents folder (`{userdocs}` / `GetFolderPath('MyDocuments')`), so OneDrive or
  folder redirection is followed.
- `Plugins\FFT` is the folder PluginBuilder's *Install Plugin* uses, so an installed machine and a dev machine
  look the same and an upgrade replaces a PluginBuilder install instead of registering the operator twice.
- Every DLL must sit flat beside `FFT.dll`: the plugin opens FFTW and oneMKL from its own directory
  (`pluginDirectory()` in `FftBackend.h`).
- The plugin list comes from the build, not from this document: `FFT.dll` plus the build's runtime manifest
  `build/FFT_runtime_dlls.txt` (written by `td_plugin_add_runtime_dll`). A new runtime file reaches both installers
  without editing them.
- The uninstaller is kept out of TouchDesigner's plugin folder on purpose: that folder then holds plugin files only.

## 3. Checks, before anything is copied

| Check | Setup.exe | install.ps1 |
|---|---|---|
| 64-bit Windows 10/11 | `ArchitecturesAllowed`, `MinVersion=10.0` | `Is64BitOperatingSystem` |
| AVX2 (+ FMA) | `IsProcessorFeaturePresent(40)`; exits with an explanation | the same, plus `Fma.IsSupported` on PowerShell 7 |
| TouchDesigner closed | WMI query; Retry/Cancel loop; **aborts when silent** (EssentiaTD's rule: `/SUPPRESSMSGBOXES` would answer Retry forever) | `Get-Process TouchDesigner*`; Enter to retry; error when non-interactive; `-Force` renames files in use aside |
| VC++ runtime ≥ the build's compiler | offers to download and run `vc_redist.x64.exe` from `aka.ms/vc14` | the same, after checking its Authenticode signature is Microsoft's |
| Downloads | SHA-256 pinned (Inno's `DownloadPage.Add`) | SHA-256 pinned (`onemkl.json`; the zip from `SHA256SUMS.txt` or the stamped hash) |

**Why the VC++ check exists.** `FFT.dll` is built `/MD`, and TouchDesigner ships no `msvcp140*.dll` of its own:
it uses the system's. Microsoft's rule is that the runtime must be at least as new as the toolset that built the
binary; ignoring it is how binaries built with MSVC 14.40+ crash in `std::mutex` on an older `msvcp140.dll`, and
`MSVCP140_ATOMIC_WAIT.dll` / `VCRUNTIME140_1.dll` do not exist at all in old redistributables. The floor is not
typed anywhere: `build_release.py` reads the compiler version CMake recorded (19.52 → runtime 14.52) and passes
it to both installers. A user who declines the update is warned and the install continues.

**The CPU check** matters because the plugin refuses to run without AVX2/FMA (every cook outputs zeros with an
error): installing it on such a machine would only produce a node that shows an error.

## 4. oneMKL: downloaded at install time, never redistributed

- `installer/onemkl.json` pins `intelmkl.redist.win-x64` **2026.1.0.226** (the version the FFT Backend toggle was
  tested with), its nuget.org URL, size (173,918,077 bytes) and SHA-256. NuGet's own SHA-512 is recorded as a
  cross-check. The package's `requireLicenseAcceptance` is false; the Intel Simplified Software License permits
  redistribution of the unmodified DLLs with its notice, and both installers install `license.txt` and
  `third-party-programs.txt` beside them.
- **14 DLLs, never the whole package.** Not installed: `mkl_intel_thread.3.dll` and `mkl_tbb_thread.3.dll` (the
  plugin forces the sequential threading layer at load) and `libimalloc.dll` (never loaded on the FFT path).
  `libiomp5md.dll` must never sit beside `FFT.dll`: TouchDesigner loads its own Intel OpenMP and two in one
  process abort; both installers delete one if they find it. All 14 are needed even though only five load on a
  given CPU: oneMKL picks its kernel DLL at run time, and a missing one is a fatal error inside oneMKL.
- Setup.exe downloads the `.nupkg` (a zip) with `CreateDownloadPage`, extracts it with
  `ArchiveExtraction=full`, and copies the 14 files through ordinary `[Files] … external` entries, so they are in
  the uninstall log. `install.ps1` extracts only the needed entries with `System.IO.Compression` (Windows
  PowerShell 5.1's `Expand-Archive` rejects a `.nupkg`). `-OneMklPackage <file>` uses a package already on disk.
- **Upgrades keep a current oneMKL.** Both installers write `oneMKL-licenses\nuget-package.txt` (the package id and
  version). On the next run, a set with all 14 DLLs and that marker, or with `mkl_rt.3.dll` at the pinned file
  version (a set copied by hand), is kept, so an update of the plugin does not download 166 MB again. Unticking the
  component (Setup.exe) or `-RemoveOneMKL` (script) removes the set. The Ready page says which of the three will
  happen, including "found in the plugin folder and will be REMOVED".
- Without oneMKL, a node with FFT Backend on runs on FFTW3 and, since v2.14.0, shows a warning naming the fix.
- **To move to a newer oneMKL:** change `package_version`, `url`, `size`, `sha256`, `nuget_sha512`,
  `file_version` and `installed_size` together in `onemkl.json`; confirm the `dlls` list against the new package
  (`runtimes/win-x64/native/`); test the toggle in TouchDesigner. Nothing else references the version.

## 5. Upgrades and uninstall

Setup.exe deletes before it copies (`[InstallDelete]`), so an upgrade never leaves a stale plugin DLL behind
(TouchDesigner would register the operator twice): `FFT.dll`, `*.dll.old*` (PluginBuilder's rename-in-place
leftovers), `libfftw3f-*.dll`, the stray files above, the oneMKL set when it is being removed or replaced, and
loose `FFT.dll` / `libfftw3f-3.3.11-avx2.dll` copies at the root of `Plugins\` from pre-installer manual installs.
`install.ps1` removes the same set and touches nothing else in the folder.

Uninstall (Apps & features, or `install.ps1 -Uninstall`, which runs Setup.exe's uninstaller first when there is
one) removes the plugin files, the oneMKL set, the examples and every folder it leaves empty. The FFTW planning
cache is kept unless the user says otherwise (a question in the interactive uninstaller, `-Purge` for the script).

## 6. Building a release

```powershell
# 1. build and test (see the README, "Building")
cmake -S PluginProjects/FFT -B PluginProjects/FFT/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build PluginProjects/FFT/build
ctest --test-dir PluginProjects/FFT/build -LE perf --output-on-failure
# 2. package (needs Inno Setup 6.5+: winget install JRSoftware.InnoSetup)
python installer/build_release.py            # or --no-installer, --fftw-source
```

`build_release.py` refuses to package when the runtime manifest names a file the build did not produce, when the
FFTW DLL does not match `FFTW_VENDORED_DLL_SHA256` in `3rdParty/fftw3/VERSION`, when `examples/` lacks the main
example, or when `install.ps1` is not pure ASCII (Windows PowerShell 5.1 reads a BOM-less script in the ANSI code
page, and `irm | iex` can choke on a BOM). With `--release` it also requires the tag to equal `v` + the
`plugin.json` version and `FFT.dll` to embed the current commit without `+dirty`.

**CI** (`.github/workflows/build.yml`): every push and pull request builds with MSVC + Ninja on `windows-latest`,
runs `ctest -LE perf` with `FFT_TEST_TIMING_SCALE=4` (a shared VM is not idle; the six wall-clock checks are
widened, never skipped), packages, and uploads `installer/dist/` as a run artifact. PluginBuilder_V2 is checked
out beside the repository at a pinned commit, as on a dev machine. A `v*` tag additionally runs
`build_release.py --release --fftw-source` and publishes a GitHub Release with `RELEASE_NOTES.md` as its body;
a tag containing `-` (`v2.14.0-rc1`) publishes a pre-release, which `releases/latest` ignores.

**Release checklist**

1. `plugin.json` version, `FFT.cpp` fallback literals, `CHANGELOG.md` entry, `RELEASE_NOTES.md` (it becomes the
   release body, and watchers are notified with the body that exists at publish time).
2. Commit, then reconfigure and rebuild so the popup's build identity is the commit, not `+dirty`; run the tests.
3. Load the build in TouchDesigner (README, "Verifying a build in TouchDesigner").
4. `git tag vX.Y.Z && git push origin vX.Y.Z`; check the run and the release's assets.
5. Install the released `TD_Custom_FFT-Setup.exe` on a clean machine (Windows Sandbox) at least once per release
   that changes the installer.

## 7. What has been tested

On the dev machine (Windows 11, i9-13900H, VC++ runtime 14.52), 2026-11-23, against the v2.14.0 assets, into
scratch folders:

| Test | Result |
|---|---|
| Setup.exe `/VERYSILENT /COMPONENTS="core,onemkl,examples"` | Installed in 15 s including the 166 MB oneMKL download (SHA-256 checked) and extraction: 6 plugin files, 14 DLLs, both licenses, the marker, the example; uninstaller in `%LOCALAPPDATA%` |
| The same again (upgrade) | No download; the oneMKL set kept; same folder reused |
| `/COMPONENTS="core,examples"` over it | The oneMKL set and its licenses removed |
| Silent uninstall | Plugin folder, examples, uninstaller and the Apps & features entry gone; wisdom cache kept |
| Setup.exe and `install.ps1` with a `TouchDesigner*.exe` process running | Both refuse (Setup exit 7, script exit 1), nothing copied |
| `install.ps1` from the extracted zip, Windows PowerShell 5.1: core; `-OneMklPackage`; re-run `-OneMKL`; `-RemoveOneMKL`; `-Uninstall` | All as designed; the re-run kept oneMKL in 1 s |
| `install.ps1` in PowerShell 7 as a script block with `-Source <zip>` | Installed |
| `… \| Invoke-Expression` with no release published (5.1 and 7) | A clear download error, and the calling shell survives (no `exit` on that path) |

**Not yet tested:** the interactive wizard pages and the VC++ update path (the machine already had 14.52); a clean
machine with TouchDesigner (Windows Sandbox); the one-liner against a published release.

## 8. Open

- **Windows Sandbox run** (audit plan P14): install TouchDesigner, then Setup.exe; create the node; check the Info
  DAT `fft_backend` row; toggle FFT Backend with and without oneMKL; uninstall; repeat with TouchDesigner running.
- **Does TouchDesigner load every DLL in the Plugins tree at start-up?** (audit plan P13). With the 14 oneMKL DLLs
  beside `FFT.dll` the dev setup works, so this is about start-up time, not correctness. If it does, oneMKL moves to
  a folder TouchDesigner does not scan and `loadBackend()` is pointed there.
- **Code signing.** Unsigned, like EssentiaTD: SmartScreen shows "unknown publisher" (More info → Run anyway). A
  certificate would go into `SignTool=` and also sign `FFT.dll`; Intel's DLLs are already signed.

## 9. Lineage: EssentiaTD's installer

[EssentiaTD](https://github.com/DarienBrito/EssentiaTD)'s `installer/windows/EssentiaTD.iss` was the model. Taken
from it: `{userdocs}\Derivative\Plugins\<name>`, `PrivilegesRequired=lowest`, x64-only, the stale-DLL wipe, the
removal of flat-layout copies, the TouchDesigner-running loop and its silent-mode rule, unversioned asset names,
the GitHub workflow shape (build → test → Inno from `pwsh`, because bash's MSYS layer mangles `iscc /D` switches →
release on a tag, `RELEASE_NOTES.md` as the body).

Added here, because this plugin has what EssentiaTD's does not (a ~456 MB optional runtime, a hard AVX2/FMA
requirement, a GPL dependency shipped as a separate DLL) or because it makes the first install easier:
components; oneMKL downloaded from its publisher with a pinned hash instead of redistributed; an upgrade that
keeps a current oneMKL; the CPU and VC++ runtime checks; a Ready page that says what will be downloaded or
removed; the uninstaller outside the plugin folder; the examples component; a script installer for
`irm | iex`, offline (`-Source`, `-OneMklPackage`) and studio (`-Yes`) use; release checks that the binary is the
tagged commit; the FFTW source attached to every release.
