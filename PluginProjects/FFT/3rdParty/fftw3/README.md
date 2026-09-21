# FFTW3 for Windows 64-bit (single precision) — vendored 3.3.11, AVX2 + FMA

Only the single-precision runtime (`fftwf_` / `libfftw3f`) is vendored. The full record of what
this build is — version, source, hashes, build recipe, the one upstream patch — is in `VERSION`
next to this file; this README is the short version and the install instructions.

```
3rdParty/fftw3/
├── VERSION                              <- the authoritative record (hashes, recipe, patch)
├── include/fftw3.h                      <- 1 KB redirector, so #include <fftw3.h> keeps working
├── include/fftw3-3.3.11-avx2.h          <- the real header, version-stamped in its name
├── include/libfftw3f-3.3.11-avx2.def    <- exports, generated from the built DLL
├── bin/libfftw3f-3.3.11-avx2.dll        <- runtime, staged next to FFT.dll by the build
├── lib/libfftw3f-3.3.11-avx2.lib        <- MSVC import library (from the .def, by lib.exe)
├── lib/libfftw3f-3.3.11-avx2.exp
└── lib/libfftw3f-3.3.11-avx2.pdb
```

Every file is named with the version and the SIMD level it was built for. That is deliberate: two
FFTW builds that differ only in SIMD produce identical results and identical `fftwf_version`
prefixes, so the file name is the only thing on disk that says which one is in play.

`td_plugin_use_fftw3(FFT VERSION 3.3.11-avx2 DYNAMIC)` in `CMakeLists.txt` (defined in
`PluginBuilder_V2/cmake/TDPlugin.cmake`) locates these files, generates the `.lib` from the `.def`
when it is missing, and stages/deploys the DLL. `DYNAMIC` means the plugin does **not** link the
import library — see "Two libraries, one ABI" below.

## Where it came from

Source: **https://fftw.org/pub/fftw/fftw-3.3.11.tar.gz** (MD5 `40ec8d0447d03b8f01f8c90aa77bd16f`),
built locally with CMake + Ninja + MSVC 19.51, `-DENABLE_AVX2=ON` (which also enables SSE2 and AVX),
`-DENABLE_THREADS=OFF`, `-DENABLE_FLOAT=ON`, single precision only. The build's own version string is
`fftw-3.3.11-sse2-avx-avx2-avx2_128`, and the DLL's SHA-256 is recorded in `VERSION`.

Two notes that cost real time to find, and are why this is built from source rather than downloaded:

* **There is no official AVX2 Windows binary.** fftw.org's Windows page publishes prebuilt 3.3.5
  DLLs (SSE2 + AVX only) — no AVX2, no 3.3.11. AVX2 has to be built.
* **Upstream's contributed `CMakeLists.txt` is wrong for 3.3.11**: it still says
  `set (FFTW_VERSION 3.3.10)`. That value becomes `PACKAGE_VERSION`, which is what `fftwf_version`
  returns *and* the version header FFTW stamps into its wisdom files, so an unpatched build reports
  itself as 3.3.10 forever. One line is patched in the scratch source tree; the block is quoted in
  `VERSION`. The same lag exists in 3.3.10 and 3.3.8 upstream.

## Two libraries, one ABI: why nothing is linked

FFTW3 and Intel oneMKL's FFTW3 compatibility interface export the *same* `fftwf_*` symbols. Only one
of them can be linked into a binary, and the choice would then be frozen at build time — no fallback
when one is missing, no way to compare them, no way to let the user pick. So **neither is linked**:
`source/FftBackend.h` resolves the chosen library with `LoadLibraryEx` + `GetProcAddress` at run
time and the engine calls through that table. `dumpbin /dependents FFT.dll` shows no FFT library
among its imports, which is the check that this really is dynamic.

That also retires the classic MSVC trap this tree used to document: with a static import,
`fftwf_version` and `fftwf_cc` are *data* and bind to the import library's jump thunk unless
`FFTW_DLL` is defined, so reading them prints machine code. `GetProcAddress` returns the address of
the real array, so the question cannot arise.

## Using Intel oneMKL instead (the FFT Backend toggle)

The node's **FFT Backend** toggle (Performance page) switches between this vendored FFTW3 build and
Intel oneMKL's FFTW3 interface, at run time, on the same node. Measured on this project's bench on an
i9-13900H (6 visible cores, AVX2, no AVX-512), same binary, `--fft 16384 --bins 16384`, only the
library differing:

| library | fft+mag | total per cook |
|---|---|---|
| FFTW3 3.3.11 AVX2, `FFTW_MEASURE` plan from wisdom | 11.94 µs | 21.06 µs |
| Intel oneMKL 2026.1.0 | 8.85 µs | 17.63 µs |

Four paired runs each way put oneMKL **13–34 % faster on the fft+mag stage** every time (absolute
times move with laptop thermals; the direction did not). oneMKL is the faster of the two on this
Intel CPU, and it is the reason the toggle exists.

**oneMKL is not redistributed here.** It is **521 MB** of Intel-licensed binaries (16 `mkl_*.dll`
plus `libimalloc.dll`, extracted from the 165.9 MB `intelmkl.redist.win-x64` 2026.1.0.226 package)
and the ISSL requires its notice files to ship with it, which is the user's call to make, not a
build script's. To enable the toggle on a machine:

1. Get the runtime DLLs — `intelmkl.redist.win-x64` from nuget.org, or the oneMKL component of the
   oneAPI toolkit (https://www.intel.com/content/www/us/en/developer/tools/oneapi/onemkl-download.html).
2. Copy these next to `FFT.dll` (into `Plugin_FFT/__Plugins__/FFT/`), all the same version:
   `mkl_rt.3.dll`, `mkl_core.3.dll`, `mkl_def.3.dll`, `mkl_mc3.3.dll`, `mkl_avx2.3.dll`,
   `mkl_avx512.3.dll`, `mkl_avx10.3.dll`, `mkl_sequential.3.dll`, `mkl_intel_thread.3.dll`,
   `mkl_tbb_thread.3.dll`, the `mkl_vml_*.3.dll` set, and `libimalloc.dll` — i.e. copy every DLL in
   the package's `runtimes/win-x64/native` directory, which is exactly the 17 above.
   *(The `.3` suffix is the ABI version oneMKL 2026.x uses; 2025.x is `.2`, 2020.4 and earlier are
   unversioned. The plugin looks for `mkl_rt.3.dll`, then `mkl_rt.2.dll`, then `mkl_rt.dll`.)*
   **Only the `mkl_rt` name matters for loading; the rest are shipped so that the code path Intel
   dispatches to exists on whatever CPU the project is opened on.** Kernel DLLs are
   architecture-specific and oneMKL loads its layer and kernels at run time.
   **`libiomp5md.dll` is not on this list and the redist does not ship one** — see the two
   deliberate decisions below. This list used to name it, which was wrong twice over: it is not in
   the package, and asking for it would have produced the Error #15 described below on purpose.
3. Copy the notices too — the package's `license.txt` and `share/doc/mkl/licensing/`, which
   require attribution (`third-party-programs.txt` is the one that matters here). The ISSL permits
   redistribution but requires these to travel with the DLLs. On this machine they live in
   `__Plugins__/FFT/oneMKL-licenses/` beside the DLLs, and `.gitignore` keeps the whole
   deployment out of the repository.
4. Open the node's Performance page and turn **FFT Backend** on. The Info DAT row `fft_backend` and
   the textport log name the library, its path and its version, so there is no doubt which one ran.

Where the plugin looks for them is **its own directory, not `build/bin`**: `loadBackendFor()` defaults
to `pluginDirectory()`, and the library is opened with `LOAD_WITH_ALTERED_SEARCH_PATH` so oneMKL finds
its 16 sibling DLLs in that same folder without any `PATH` entry. Staging them in `build/bin/Release`
(what the bench and the tests resolve against) therefore proves the backend works but does **not**
make it work in TouchDesigner — that needs the copy into `__Plugins__/FFT/`.

If the DLLs are not present the plugin logs which file it looked for and falls back to FFTW3 for
that node — it never leaves the node without a plan.

### Two things this project does deliberately with oneMKL

* **`MKL_Set_Threading_Layer(MKL_THREADING_SEQUENTIAL)` is called the moment the library loads.**
  oneMKL otherwise defaults to its Intel OpenMP layer, which brings `libiomp5md.dll` into the host
  process. **TouchDesigner already ships and loads its own `libiomp5md.dll`**, and two Intel OpenMP
  runtimes in one process is the documented *"Error #15: Initializing libiomp5md.dll, but found
  libiomp5md.dll already initialized"* abort. The ISSL forbids modifying or working around the DLLs
  themselves, so choosing the sequential layer at load is the only fix available to a plugin. It
  also matches the node's threading model: this plugin parallelises across *channels*, never inside
  a transform, and the Async toggle's whole promise is that "off" means one thread. Verified both
  ways on the real binary — with the default layer `mkl_intel_thread.3.dll` is resident; with the
  switch, neither it nor `libiomp5md.dll` is loaded. `fft_tests` asserts this.
* **The planner policy and the wisdom cache are reported as inapplicable, not silently ignored.**
  oneMKL accepts `FFTW_MEASURE` / `PATIENT` / `WISDOM_ONLY` and ignores them, and its wisdom
  save/load functions return 0 without touching a file (Intel lists them as "empty"). Planning costs
  a few hundred microseconds regardless of flag. So the engine plans with `FFTW_ESTIMATE` on that
  backend, says so in the plan line, and the Info DAT notes that the library picks its own plan —
  rather than printing "measuring in background" and never delivering an upgrade.

License: FFTW is GPL. oneMKL is under the Intel Simplified Software License and is not distributed
with this project. See §"License / third party" in the project README.
