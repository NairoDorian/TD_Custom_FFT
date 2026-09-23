# Plugin_FFT — Windows installer plan

**Date:** 2026-09-23. **Plugin version:** 2.12.0.

**Goal:** one `.exe` that installs everything the FFT CHOP needs on another Windows machine into
`C:\Users\<user>\Documents\Derivative\Plugins\FFT`, with no admin rights and no manual file copying.

**Model:** EssentiaTD's Inno Setup installer (`EssentiaTD/installer/windows/EssentiaTD.iss`), adapted to
what this plugin has and EssentiaTD does not:
- two FFT runtimes, one of them ~456 MB and optional;
- a hard AVX2/FMA requirement;
- a GPL dependency shipped as a separate DLL.

Nothing here is implemented yet. §8 is the build order.

---

## 1. What has to land on the target machine (measured, not assumed)

Measured on this machine with `dumpbin /dependents` and a live module list of `fft_bench` with each backend.

| File | Size | Needed when | Imports |
|---|---|---|---|
| `FFT.dll` | 0.27 MB | always | `MSVCP140`, `MSVCP140_ATOMIC_WAIT`, `VCRUNTIME140`, `VCRUNTIME140_1`, UCRT (`api-ms-win-crt-*`), `KERNEL32` |
| `libfftw3f-3.3.11-avx2.dll` | 3.1 MB | always (default backend; the name is fixed by `td_plugin_use_fftw3(... VERSION 3.3.11-avx2)`) | `VCRUNTIME140`, UCRT, `KERNEL32` |
| `mkl_rt.3.dll`, `mkl_core.3.dll`, `mkl_sequential.3.dll` | 118 MB | only with **FFT Backend = oneMKL** | `KERNEL32` (+ `mkl_core`) |
| `mkl_def`, `mkl_mc3`, `mkl_avx2`, `mkl_avx512`, `mkl_avx10` (`.3.dll`) | 269 MB | oneMKL: loads the one matching the CPU | `KERNEL32`, `mkl_core` |
| `mkl_vml_def`, `_mc3`, `_avx2`, `_avx512`, `_avx10`, `_cmpt` (`.3.dll`) | 80 MB | oneMKL: loads the one matching the CPU | `KERNEL32`, `mkl_core` |
| `oneMKL-licenses/license.txt`, `third-party-programs.txt` | 67 KB | whenever any `mkl_*` ships (Intel license requirement) | — |

Measured on the i9-13900H with oneMKL active: exactly `mkl_rt`, `mkl_core`, `mkl_sequential`, `mkl_avx2` and
`mkl_vml_avx2` load (~177 MB). The other CPU variants are there for other machines.

**Not shipped:** `mkl_intel_thread`, `mkl_tbb_thread` and `libimalloc.dll`. These were removed from the
deployment on 2026-09-23. The plugin calls `MKL_Set_Threading_Layer(SEQUENTIAL)` at load, so the two
threading layers can never be used, and `libimalloc` never loads on the FFT path. The details are in
`PluginProjects/FFT/3rdParty/fftw3/README.md`, "Using Intel oneMKL".

**Not shipped either:** the FFTW wisdom file. It is machine-specific timing data, created on first use in
`%LOCALAPPDATA%\TD_Custom_FFT\fftwf_wisdom.txt`.

### Nothing else is needed, with one caveat: the VC++ runtime

- `FFT.dll` is built `/MD`, so it needs the **Visual C++ 2015–2022 Redistributable (x64)**.
- TouchDesigner's own `bin/` folder does **not** contain `msvcp140*.dll` or `vcruntime140*.dll`. Checked
  in `TouchDesigner.2025.33230\bin`: none there. TD relies on the system-wide redistributable, which its own
  installer puts in `System32`.
- **`MSVCP140_ATOMIC_WAIT.dll` and `VCRUNTIME140_1.dll` only exist in newer redistributables (VS 2019 16.8+ /
  2022).** A machine with TD 2025 installed has them. A machine with an old TD, or a stripped Windows image,
  may not, and then TD refuses to load the plugin with a generic "module not found" error.
- The installer must check for them (§4.3).

---

## 2. What EssentiaTD's installer does, and what we take from it

`EssentiaTD/installer/windows/EssentiaTD.iss` is 99 lines. Everything in it applies here:

| EssentiaTD choice | Why it matters | Take it? |
|---|---|---|
| **Inno Setup 6.3+**; builds with `iscc /DAppVersion=... /DPluginDir=... /DOutputDir=...` | Plain text script, free, scriptable in CI (`choco install innosetup` on the GitHub runner) | **Yes** |
| `DefaultDirName={userdocs}\Derivative\Plugins\Essentia` | `{userdocs}` resolves the **real** Documents folder, including OneDrive or folder redirection. PluginBuilder's own install step uses `os.path.expanduser('~') + '\Documents'` (`PluginBuilderExt.py:1084`), which misses a redirected Documents folder. | **Yes**: `{userdocs}\Derivative\Plugins\FFT`. Also fix PluginBuilder to use `SHGetKnownFolderPath(FOLDERID_Documents)`. |
| `PrivilegesRequired=lowest` | Per-user install: no admin rights, no UAC prompt | **Yes** |
| `ArchitecturesAllowed/InstallIn64BitMode=x64compatible` | TD is x64-only | **Yes** |
| `[InstallDelete] {app}\*.dll`, `*.dll.old` before copying | Upgrades never leave orphan DLLs. TD would register duplicate operators from leftovers. | **Yes**, plus our known leftovers (§4.4) |
| `[InstallDelete]` of the older flat-layout copies | Earlier instructions said to copy DLLs loose into `Plugins\` | **Yes**: remove any loose `Plugins\FFT.dll` |
| `PrepareToInstall` + WMI `Win32_Process WHERE Name LIKE 'TouchDesigner%'`, Retry/Cancel loop | A running TD keeps the DLLs mapped, so overwrites fail or the old module stays in use | **Yes**, verbatim, including its silent-mode rule: in `/SILENT` it **aborts** instead of looping, because `/SUPPRESSMSGBOXES` answers Retry forever |
| Unversioned `OutputBaseFilename=EssentiaTD-Setup` | Keeps the `releases/latest/download/...` link stable | **Yes**: `FFT-Setup.exe` (the version is in the metadata and wizard title) |
| `LicenseFile=..\..\LICENSE` | Shows the license page | **Yes, but this repo has no LICENSE file.** Add one first (§6). |
| Unsigned; the README tells users to click through SmartScreen | Code signing costs money | Same for now; optional signing in §7 |

**The one thing EssentiaTD does differently at build level:** it links Essentia **statically**
(`/WHOLEARCHIVE:essentia.lib`, 107 MB), so each plugin is a single self-contained DLL and the installer only
copies `*.dll` from one folder. We cannot do that, and should not:
- FFTW is GPL. A dynamic, separately replaceable DLL keeps the licensing clean.
- oneMKL is chosen at run time by the FFT Backend toggle, and both libraries export the same `fftwf_*`
  symbols.

So our installer copies a **list** of files, not a folder glob (§3).

---

## 3. Installer design

### 3.1 Components (the core/optional split EssentiaTD doesn't need)

```
[Types]
Name: "full";    Description: "FFT CHOP + Intel oneMKL backend (recommended on Intel CPUs, ~456 MB)"
Name: "compact"; Description: "FFT CHOP only (FFTW3 backend, ~3.5 MB)"

[Components]
Name: "core";   Description: "FFT CHOP (FFT.dll + FFTW3 3.3.11 AVX2)"; Types: full compact; Flags: fixed
Name: "onemkl"; Description: "Intel oneMKL FFT backend (18-27 % faster FFT on this i9-13900H)"; Types: full
```

- The plugin works without the `onemkl` component. With the toggle on and the DLLs missing, it logs the
  file it looked for and falls back to FFTW3 (`FftBackend.h`).
- **Unticking `onemkl` on an upgrade must delete the existing `mkl_*` files.** Otherwise a stale oneMKL of
  another version stays behind. Handle it with `[InstallDelete]` guarded by `Components: not onemkl`
  (Inno supports `Components`/`Check` on `[InstallDelete]` entries).

### 3.2 Files, from an explicit list

```
[Files]
; core
Source: "{#StageDir}\FFT.dll";                     DestDir: "{app}"; Components: core;   Flags: ignoreversion
Source: "{#StageDir}\libfftw3f-3.3.11-avx2.dll";   DestDir: "{app}"; Components: core;   Flags: ignoreversion
Source: "{#StageDir}\licenses\FFTW-COPYING.txt";   DestDir: "{app}\licenses"; Components: core
Source: "{#StageDir}\licenses\Plugin_FFT-LICENSE.txt"; DestDir: "{app}\licenses"; Components: core
; oneMKL (14 DLLs + notices), only when the staging dir has them
#ifdef WithMkl
Source: "{#StageDir}\mkl\*.dll";                   DestDir: "{app}"; Components: onemkl; Flags: ignoreversion
Source: "{#StageDir}\mkl\oneMKL-licenses\*";       DestDir: "{app}\oneMKL-licenses"; Components: onemkl
#endif
```

- The core list comes from the build's own runtime manifest, `build/FFT_runtime_dlls.txt`, which
  `td_plugin_add_runtime_dll` writes (it currently holds exactly `libfftw3f-3.3.11-avx2.dll`), plus
  `FFT.dll`. A future runtime dependency then reaches the installer without editing the `.iss` file.
- The staging script (§5) writes the `[Files]` core lines from that manifest into an include file.
- Never glob `__Plugins__\FFT\*.dll`: that folder is a dev deployment and has held `.old` files and removed
  runtimes before.

### 3.3 Install folder and layout

- The destination is `{userdocs}\Derivative\Plugins\FFT\`, the folder PluginBuilder's Install step already
  uses, so an installed machine and a dev machine look the same.
- The plugin resolves FFTW/oneMKL from **its own directory** (`pluginDirectory()` +
  `LOAD_WITH_ALTERED_SEARCH_PATH`), so every DLL must sit **flat next to `FFT.dll`**. Licenses can go in
  subfolders.
- **Open question to verify before shipping:** TouchDesigner scans the Plugins folder and its
  subfolders for plugin DLLs (EssentiaTD relies on the subfolder scan). Does TD `LoadLibrary` every DLL it
  finds, including the 14 `mkl_*` files, at start-up just to probe for `FillCHOPPluginInfo`?
  - If it does, the full install costs start-up time and address space on every TD launch, even for
    projects that never use the node.
  - **To test:** time a TD cold start with and without the oneMKL set, and check TD's module list with
    Process Explorer.
  - **If it does:** move oneMKL to a sibling folder that TD does not scan, e.g.
    `Documents\Derivative\FFT-runtime\onemkl\`, and point `loadBackend()` there via a registry value or
    an env var the installer sets.
  - Today's dev setup already has them next to `FFT.dll` and works, so this is about start-up cost, not
    correctness.

---

## 4. Pre-install checks (`[Code]`)

### 4.1 TouchDesigner running

EssentiaTD's `TouchDesignerRunning()` / `PrepareToInstall()`, unchanged: the WMI query, the Retry/Cancel
loop, and the abort in silent mode.

### 4.2 CPU: AVX2 + FMA (EssentiaTD has no such requirement; we have a hard one)

- The plugin refuses to run without AVX2/FMA: `myCpuOk`, which makes every cook output zeros with an error.
  Installing it on such a machine gives a node that only ever shows an error.
- `IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE = 40)` from `kernel32` answers AVX2, and
  Inno can call it:
  ```
  function IsProcessorFeaturePresent(Feature: DWORD): BOOL; external 'IsProcessorFeaturePresent@kernel32.dll stdcall';
  ```
  Windows exposes no FMA flag through that API, but every x86 CPU with AVX2 also has FMA3 (Haswell+, Zen+).
- On failure: show an error and abort ("This CPU has no AVX2; the FFT CHOP requires it"). In silent mode,
  return the message.

### 4.3 VC++ runtime

- Check that `{sys}\msvcp140_atomic_wait.dll` and `{sys}\vcruntime140_1.dll` exist. With the 64-bit install
  mode on, `{sys}` is the 64-bit System32.
- **Recommended:** if either is missing, offer to download and run Microsoft's `vc_redist.x64.exe`
  (`https://aka.ms/vs/17/release/vc_redist.x64.exe`) with Inno 6.1+'s `CreateDownloadPage`. That step needs
  elevation; `vc_redist` asks for it by itself.
- **Alternative:** bundle `vc_redist.x64.exe` (~25 MB). It is simpler offline, but bigger and outdated
  over time.
- **Alternative:** build `FFT.dll` with `/MT` (static CRT). That removes the check entirely, but it gives the
  plugin its own heap, separate from TD's. That is fine here because no memory crosses the boundary, but it
  must be re-verified. FFTW's DLL still needs `VCRUNTIME140` either way, unless it is also rebuilt `/MT`.
  Not recommended as the first step.

### 4.4 Clean-up of known leftovers (`[InstallDelete]`)

```
Type: files; Name: "{app}\*.dll"                      ; everything we own is re-copied
Type: files; Name: "{app}\*.dll.old"                  ; PluginBuilder's rename-in-place leftovers
Type: files; Name: "{app}\*.dll.old.*"
Type: files; Name: "{userdocs}\Derivative\Plugins\FFT.dll"   ; a loose copy from manual installs
```

The `{app}\*.dll` wipe also covers the files dropped on 2026-09-23 (`libfftw3f-3.dll`, `libimalloc.dll`,
`mkl_intel_thread.3.dll`, `mkl_tbb_thread.3.dll`).

---

## 5. Build pipeline (reproducible, one command)

The new files:

```
Plugin_FFT/installer/
  FFT.iss               the Inno script (§3, §4)
  build_installer.py    stage + iscc, the single entry point
  licenses/             FFTW COPYING (GPL-2.0), the Plugin_FFT LICENSE
```

`build_installer.py`:
1. Reads the version from `PluginProjects/FFT/plugin.json`. It is the single source of truth, already used
   for `FFT_VERSION_*`.
2. Optionally builds and tests: `cmake --build build --config Release` and
   `ctest --test-dir build -LE perf`. It refuses to package a build whose tests fail.
3. Stages into `installer/stage/`:
   - `FFT.dll` plus every DLL in `build/FFT_runtime_dlls.txt`, copied from `build/bin/Release`;
   - the license files;
   - with `--with-mkl <dir>`, the 14 `mkl_*` files from the given directory (the dev
     `__Plugins__/FFT/`, or an extracted `intelmkl.redist.win-x64` 2026.1.0.226 nupkg) plus
     `oneMKL-licenses/`. It **verifies all 14 are present and share one version**, reading the file
     version resources.
4. Writes `stage/files_core.iss` from the manifest.
5. Runs `iscc /DAppVersion=<v> /DStageDir=stage /DOutputDir=out [/DWithMkl] FFT.iss`.
6. Writes `out/FFT-Setup.exe` (with oneMKL) or `out/FFT-Setup-core.exe`, plus a `SHA256SUMS.txt`.

Tooling:
- Install Inno Setup with `winget install JRSoftware.InnoSetup`. It isn't installed on this machine; winget
  1.29 is.
- Optional: a PluginBuilder **"Build Installer"** pulse that runs `build_installer.py` off-thread through
  the existing `BuildRunner` (callable jobs). It would sit next to *Bake for Performance*.

### CI (later)

- A `windows-latest` GitHub Actions job: configure, build, `ctest -LE perf`, then
  `choco install innosetup` and `build_installer.py`.
- On a `v*` tag, attach `FFT-Setup-core.exe` to a GitHub Release. EssentiaTD's
  `.github/workflows/build.yml` does the same.
- **CI ships the core installer only.** The oneMKL runtime is 166 MB compressed, and redistributing it is
  a licensing decision to make once, deliberately. When making it, either:
  - the full installer is built locally with `--with-mkl`, or
  - the core installer offers to download `intelmkl.redist.win-x64` from nuget.org at install time
    (`CreateDownloadPage`) and extract the 14 files.
    - A `.nupkg` is a zip. Inno 6.4+ can extract archives from `[Code]` (verify the exact API on the version
      installed), with `powershell Expand-Archive` in `[Run]` as the fallback.
    - Pin the package version, and verify its SHA-256 before extracting.

---

## 6. Licensing obligations (do these before the first public installer)

1. **Add a `LICENSE` for Plugin_FFT.** The repo has none, and an installer needs one for `LicenseFile=`. FFTW
   is GPL-2.0-or-later and is loaded dynamically, so the combined distribution falls under the GPL (the
   README already says so). The simplest consistent choice is GPL-3.0-or-later or GPL-2.0-or-later for the
   plugin.
2. **Ship FFTW's `COPYING`** (GPL text) with `libfftw3f-3.3.11-avx2.dll`, and point to the corresponding
   source: `3rdParty/fftw3/VERSION` records the tarball URL, its MD5 and the one patch. GPL-2.0 §3 requires
   the source offer when distributing binaries.
3. **oneMKL** (Intel Simplified Software License): redistribution is allowed with the DLLs unmodified and
   un-renamed, and `license.txt` + `third-party-programs.txt` must ship with them. They are already staged in
   `__Plugins__/FFT/oneMKL-licenses/` (67 KB).
4. The installer's license page shows the Plugin_FFT license. The third-party notices are installed to
   `{app}\licenses\` and `{app}\oneMKL-licenses\`, and listed on the wizard's info-before page.

---

## 7. Optional polish

- **Code signing:** `SignTool=` in the `.iss` with an OV/EV certificate removes the SmartScreen "unknown
  publisher" prompt. EssentiaTD ships unsigned and documents the click-through instead. Sign `FFT.dll` as
  well as the installer; the Intel DLLs are already Intel-signed.
- **Uninstall:** remove `{app}` if it is empty. Leave `%LOCALAPPDATA%\TD_Custom_FFT\` (the wisdom cache)
  unless the user ticks "Also remove the FFT planning cache".
- **Show in the wizard:** the installed version and backend summary, and a "Open the Plugins folder"
  checkbox on the finish page.
- **Silent install for studios:** `FFT-Setup.exe /VERYSILENT /SUPPRESSMSGBOXES /COMPONENTS="core,onemkl"`.
  This works because of EssentiaTD's silent-mode rule (§4.1).
- **Uninstall stale PluginBuilder copies:** if `{app}` holds a `.toe`-adjacent dev layout, don't delete
  anything we didn't install. Only files matching our lists are touched.

---

## 8. Build order

1. Add `LICENSE` and `installer/licenses/FFTW-COPYING.txt` (§6). Commit.
2. Write `installer/FFT.iss`: core component only, §4.1 + §4.2 + §4.4 checks, `{userdocs}` path.
   `winget install JRSoftware.InnoSetup`.
3. Write `installer/build_installer.py`: stage from the manifest, version from `plugin.json`, tests gate.
4. Test in **Windows Sandbox** (a clean, disposable Windows):
   - install TD 2025 in it, then run `FFT-Setup-core.exe`;
   - open TD and create the FFT CHOP, then check the Info DAT `fft_backend` row;
   - uninstall and verify the folder is gone;
   - repeat with TD running, to hit the Retry/Cancel path.
5. Add the `onemkl` component + `--with-mkl`. Test: toggle FFT Backend on, confirm `mkl_rt.3.dll` in the
   Info DAT; uninstall; reinstall **core only** and confirm the `mkl_*` files were removed.
6. Answer §3.3's open question (TD start-up with 456 MB of DLLs in the scanned folder). Move oneMKL out of
   the scanned tree if it costs start-up time.
7. VC++ runtime check with download (§4.3).
8. CI job and GitHub Release (§5). Signing (§7) when there's a certificate.

## 9. File checklist for a release

| Installed path under `Documents\Derivative\Plugins\FFT\` | Source | Component |
|---|---|---|
| `FFT.dll` | `build/bin/Release/FFT.dll` | core |
| `libfftw3f-3.3.11-avx2.dll` | `3rdParty/fftw3/bin/` (via the runtime manifest) | core |
| `licenses/Plugin_FFT-LICENSE.txt`, `licenses/FFTW-COPYING.txt` | `installer/licenses/` | core |
| `mkl_rt/core/sequential.3.dll`, `mkl_{def,mc3,avx2,avx512,avx10}.3.dll`, `mkl_vml_{def,mc3,avx2,avx512,avx10,cmpt}.3.dll` | `intelmkl.redist.win-x64` 2026.1.0.226 `runtimes/win-x64/native/` | onemkl |
| `oneMKL-licenses/license.txt`, `third-party-programs.txt` | same package | onemkl |

Related notes:
- `ESSENTIATD_LESSONS_FOR_PLUGIN_FFT_2026-09-23.md` (what else to take from EssentiaTD);
- `PluginProjects/FFT/3rdParty/fftw3/README.md` ("Using Intel oneMKL", now updated to the 14-file set);
- `AUDIT_AND_PLAN_2026-09-23.md`.
