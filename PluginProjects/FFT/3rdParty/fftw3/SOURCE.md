# FFTW 3.3.11 — corresponding source for the vendored binary

`bin/libfftw3f-3.3.11-avx2.dll` (and `lib/`, `include/`) are FFTW 3.3.11, licensed under the GNU GPL version 2
or any later version ([`COPYING`](COPYING)). This file is the written direction to the complete corresponding
source, as the GPL requires for a binary distribution (GPLv3 section 6(d), which FFTW's "or any later version"
makes available; under GPLv2 section 3 the same material is offered below).

## The source

1. **Upstream tarball (unmodified):** <https://fftw.org/pub/fftw/fftw-3.3.11.tar.gz>
   MD5 `40ec8d0447d03b8f01f8c90aa77bd16f` (recorded in [`VERSION`](VERSION)).
2. **One patch:** [`fftw-3.3.11-cmake-version.patch`](fftw-3.3.11-cmake-version.patch) — upstream's
   contributed `CMakeLists.txt` still says `3.3.10`; the patch makes the build report `3.3.11` (see `VERSION`
   for why). It is the only source change.
3. **Build script:** [`build_fftw.bat`](build_fftw.bat) — the exact CMake configuration used (MSVC x64,
   Release, shared, single precision, SSE2 + AVX + AVX2, no threads, no OpenMP, no Fortran). Run it from a
   folder containing the extracted, patched `fftw-3.3.11/` tree (adjust the Visual Studio path if yours
   differs). The import library is regenerated from the DLL's exports by
   `td_plugin_use_fftw3()` in PluginBuilder_V2's `cmake/TDPlugin.cmake`.

The resulting DLL's SHA-256 is recorded in `VERSION` (`FFTW_VENDORED_DLL_SHA256`).

## Availability

The maintainer keeps the tarball above, the patch and the script available for as long as this binary is
distributed: a copy of the tarball is attached to every GitHub release of this project that ships the DLL.
On request (an issue on <https://github.com/NairoDorian/TD_Custom_FFT>), the complete source is provided at no
charge beyond the cost of physically performing the transfer, for at least three years after the last
distribution of the binary.
