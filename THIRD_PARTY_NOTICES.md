# Third-party components

`FFT.dll` is this project's code (see [`NOTICE.md`](NOTICE.md) and [`LICENSE`](LICENSE)). The components
below are separate works under their own licenses. The build deploys this file, the plugin's license and
FFTW's license next to `FFT.dll` (`__Plugins__/FFT/`), so **Install Plugin** and any copy of that folder
carry them.

## FFTW 3.3.11 — the default FFT backend

- **License:** GNU General Public License, version 2 or (at your option) any later version. Full text:
  [`PluginProjects/FFT/3rdParty/fftw3/COPYING`](PluginProjects/FFT/3rdParty/fftw3/COPYING), deployed as
  `FFTW3-COPYING.txt`. Copyright (c) 2003, 2007-14 Matteo Frigo; Copyright (c) 2003, 2007-14 Massachusetts
  Institute of Technology.
- **What is distributed:** `libfftw3f-3.3.11-avx2.dll` (single precision, SSE2/AVX/AVX2 codelets, no threads,
  no OpenMP), its import library, `.def` and headers, in `PluginProjects/FFT/3rdParty/fftw3/`.
- **Corresponding source:** the unmodified upstream tarball plus one one-line patch and the build script —
  everything needed to rebuild the identical DLL is listed in
  [`PluginProjects/FFT/3rdParty/fftw3/SOURCE.md`](PluginProjects/FFT/3rdParty/fftw3/SOURCE.md).
- **How the plugin uses it:** `FFT.dll` does not link FFTW. At run time it loads *a* library that implements
  the FFTW3 C API (`LoadLibrary` + `GetProcAddress`, `source/FftBackend.h`): this FFTW build, or Intel
  oneMKL's independent implementation of the same API. FFTW is shipped as a separate, replaceable DLL; you can
  delete it and use oneMKL, or drop in your own FFTW build.
- **If you redistribute FFT.dll together with FFTW in a closed-source or commercial product** and want
  certainty rather than the arrangement above, FFTW is also available under a non-GPL commercial license from
  MIT (see <https://www.fftw.org/>). The alternative that needs no FFTW at all is to ship the oneMKL backend
  only.

## Intel oneMKL — optional backend (FFT Backend toggle on)

- **License:** Intel Simplified Software License (redistribution of the unmodified, un-renamed binaries is
  permitted). Not part of this repository or of any release: the installers (`TD_Custom_FFT-Setup.exe`,
  `installer/install.ps1`) download the pinned `intelmkl.redist.win-x64` package from nuget.org on the user's
  machine, check its SHA-256 and install its 14 runtime DLLs next to `FFT.dll`, with `license.txt` and
  `third-party-programs.txt` in `oneMKL-licenses/` (`installer/onemkl.json`; by hand: `3rdParty/fftw3/README.md`).
- **If you redistribute it**, ship oneMKL's `license.txt` and `third-party-programs.txt` with the DLLs — the
  plugin folder keeps them in `oneMKL-licenses/`.

## The installer (`TD_Custom_FFT-Setup.exe`)

- Built with **Inno Setup** (Copyright (C) 1997-2026 Jordan Russell, portions Martijn Laan;
  <https://jrsoftware.org/isinfo.php>), under the Inno Setup License, which permits redistribution of the compiled
  setup with its copyright notices kept in place (they are, in the setup's own About box and resources).
- The setup embeds Inno's `is7z.dll` (`ArchiveExtraction=full`, used to unpack the oneMKL `.nupkg`): a recompiled
  **7-Zip** `7z.dll` by Igor Pavlov, under the GNU LGPL 2.1 or later, with the unRAR license restriction on its RAR
  code (it may not be used to re-create the RAR compression algorithm). Source: <https://www.7-zip.org/> and Inno
  Setup's own build of it in <https://github.com/jrsoftware/issrc>.
- `install.ps1`, `Install.cmd` and `build_release.py` are this project's code.

## TouchDesigner C++ SDK (Derivative Inc.)

- **License:** Derivative's Shared Use License (the notice at the top of each header). Use is limited to
  TouchDesigner; redistribution of modified versions is allowed if the notice is kept.
- The headers are not in this repository; the build takes them from `../PluginBuilder_V2/include/`.
  `source/FFT.cpp` and `source/Parameters.cpp` started from Derivative's CPlusPlus CHOP sample and keep its
  notice. NOTICE.md's additional permission is what allows this GPL program to be combined with them.

## Build-time only (not distributed in the plugin)

- **PluginBuilder_V2** (<https://github.com/NairoDorian/PluginBuilder_V2>, checked out beside this repository;
  MIT): the CMake module and build tooling.
- The test and benchmark executables (`tests/`, `bench/`) are this project's code under the same terms as the
  plugin.
