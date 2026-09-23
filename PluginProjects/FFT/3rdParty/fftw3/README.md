# FFTW3 for Windows 64-bit (single precision) — vendored 3.3.11, AVX2 + FMA

This directory is the FFTW build the plugin loads to compute its FFT. The plugin prepares a window,
hands a block of floats to FFTW and reads back the complex bins; it does not implement the transform
itself. Only the single-precision runtime (`fftwf_` / `libfftw3f`) is vendored. `VERSION` next to
this file is the authoritative record (hashes, source MD5, flags, the one upstream patch); this
README explains how the build uses it, how to rebuild or replace it, and how the oneMKL alternative
is deployed.

The tree is consumed through the sibling [`PluginBuilder_V2`](../../../../../PluginBuilder_V2) CMake
module: `td_plugin_use_fftw3(FFT VERSION 3.3.11-avx2 DYNAMIC)` in
[`PluginProjects/FFT/CMakeLists.txt`](../../CMakeLists.txt). If you change the build tag, the file
names or the link mode, update this README and `td_plugin_use_fftw3` in
`PluginBuilder_V2/cmake/TDPlugin.cmake` together, then rebuild via `PluginBuilder.tox` or
`python PluginBuilder_V2/dev/ci.py --project PluginProjects/FFT`.

## Contents

1. [At a glance](#at-a-glance)
2. [What is in this directory](#what-is-in-this-directory)
3. [How the build finds and uses this](#how-the-build-finds-and-uses-this)
4. [Rebuilding this exact library from source](#rebuilding-this-exact-library-from-source)
5. [Replacing it with a different FFTW build](#replacing-it-with-a-different-fftw-build)
6. [The CPU requirement is not universal](#the-cpu-requirement-is-not-universal)
7. [Where it came from](#where-it-came-from)
8. [Two libraries, one ABI: why nothing is linked](#two-libraries-one-abi-why-nothing-is-linked)
9. [Using Intel oneMKL instead (the FFT Backend toggle)](#using-intel-onemkl-instead-the-fft-backend-toggle)
10. [License](#license)

## At a glance

| question | answer |
|---|---|
| What is vendored here? | FFTW **3.3.11**, **single precision only** — no double or long-double build |
| Which exact build? | A locally built **Windows x64** DLL from the upstream tarball, **SSE2 + AVX + AVX2 codelets**, **threading and OpenMP off** |
| Built with what? | CMake + Ninja + **MSVC 19.51** (VS 18.10.1 Community), Release, `x64` |
| Build tag on disk | **`3.3.11-avx2`**, so every artifact is named `libfftw3f-3.3.11-avx2.*` |
| Source | `https://fftw.org/pub/fftw/fftw-3.3.11.tar.gz` (MD5 `40ec8d0447d03b8f01f8c90aa77bd16f`), built locally — [why](#where-it-came-from) |
| How the plugin uses it | Deployed next to `FFT.dll` and opened **at run time** by `source/FftBackend.h`; nothing links it — [why](#two-libraries-one-abi-why-nothing-is-linked) |
| CPU requirement | **AVX2 and FMA** — [what that excludes](#the-cpu-requirement-is-not-universal) |

## What is in this directory

```
3rdParty/fftw3/
├── README.md                                    <- this file
├── VERSION                                      <- the authoritative record (hashes, recipe, patch)
├── include/
│   ├── fftw3.h                                  <- 1 KB redirector, so #include <fftw3.h> keeps working
│   └── fftw3-3.3.11-avx2.h                      <- the real header, version-stamped in its name
├── lib/
│   ├── libfftw3f-3.3.11-avx2.def                <- exports, generated from the built DLL
│   ├── libfftw3f-3.3.11-avx2.lib                <- MSVC import library (from the .def, by lib.exe)
│   └── libfftw3f-3.3.11-avx2.exp                <- side product of generating the .lib
└── bin/
    └── libfftw3f-3.3.11-avx2.dll                <- runtime, staged next to FFT.dll by the build
```

* The `.def` lives in `lib/`: `td_plugin_use_fftw3()` reads `lib/<stem>-<tag>.def`.
* No `.pdb` is shipped; nothing in the build or the loader looks for one.
* Every file carries the version and SIMD level. Two FFTW builds that differ only in SIMD produce
  identical results and identical `fftwf_version` prefixes, so the file name is the only thing on
  disk that says which one is in play.

## How the build finds and uses this

| step | who | what happens |
|---|---|---|
| 1 | `PluginProjects/FFT/CMakeLists.txt` | calls `td_plugin_use_fftw3(FFT VERSION 3.3.11-avx2 DYNAMIC)` |
| 2 | `_td_find_3rdparty()` | finds this directory, trying `<plugin source>/3rdParty/fftw3`, then `${TD_SAMPLES_DIR}/3rdParty/fftw3`, then `${PLUGIN_BUILDER_DIR}/3rdParty/fftw3`; the first that exists wins, so this plugin-local copy wins |
| 3 | `td_plugin_use_fftw3()` | combines the stem `libfftw3f` with the tag: `libfftw3f-3.3.11-avx2` |
| 4 | `td_plugin_use_fftw3()` | adds `include/` to the target and defines `TD_PLUGIN_HAS_FFTW3=1` (plus `TD_PLUGIN_FFTW3_DYNAMIC=1` under `DYNAMIC`) |
| 5 | `td_plugin_use_fftw3()` | uses `lib/<stem>-<tag>.lib` as-is when it exists and the `.def`'s `LIBRARY` line names `<stem>-<tag>.dll`; otherwise (MSVC only) writes a corrected `.def` and runs `lib.exe` into `<build>/td_deps/fftw3/`. Nothing is ever written into this directory |
| 6 | `td_plugin_add_runtime_dll()` | copies `bin/<stem>-<tag>.dll` next to `FFT.dll` in the build output and records it in `<build>/FFT_runtime_dlls.txt`. A standalone build also copies it into `Plugin_FFT/__Plugins__/FFT/`; under `PluginBuilder.tox` (`PLUGINBUILDER_BUILD` set) PluginBuilder stages exactly the files that list names |
| 7 | `source/FftBackend.h`, at run time | `loadBackendFrom()` opens the DLL from the plugin's own directory and fills an `FftApi` table of function pointers |

Step 7 in detail:

```
the Backend parameter is an int
  -> backendById()        picks one of the two FftBackendInfo descriptors
  -> cachedBackend()      returns the process-wide FftBackend for it, loading it the first time
  -> loadBackend()        walks the descriptor's dlls[] list, one file name at a time:
                          libfftw3f-3.3.11-avx2.dll, then libfftw3f-3.dll, then fftw3f.dll
  -> loadBackendFrom()    one LoadLibraryExA plus a GetProcAddress per symbol, filling an FftApi
  -> the FftApi table     every engine call goes through this table; no library symbol is named directly
```

* `loadBackend()`'s directory argument defaults to `pluginDirectory()`, the folder `FFT.dll` lives
  in, so the deployed DLL must sit beside `FFT.dll`, not in `build/bin`.
* The DLL names are a preference list (`kFftw3Dlls` in `source/FftBackend.h`): the first that opens
  wins. A build with a different file name is found only if its name is added there.
* The version is pinned for reporting, not for loading: `kFftw3Backend` sets
  `expectedVersion = "3.3.11"`. A library reporting anything else still runs, and
  `describeBackend()` appends a `** MISMATCH` warning to the log line and the Info DAT.

## Rebuilding this exact library from source

You need Visual Studio (`x64` toolchain), CMake and Ninja, in a shell where `vcvars64.bat` has run.
The short form of this recipe is in `VERSION`.

**1. Get the source** — `https://fftw.org/pub/fftw/fftw-3.3.11.tar.gz` — and check its MD5 is
`40ec8d0447d03b8f01f8c90aa77bd16f`.

**2. Patch one line (required).** Upstream's contributed `CMakeLists.txt` says
`set (FFTW_VERSION 3.3.10)`; change it to `3.3.11`. That value becomes `PACKAGE_VERSION` in
`config.h`, which is both the string `fftwf_version` returns and the version FFTW stamps into its
wisdom files. Unpatched, a 3.3.11 build reports itself as 3.3.10 and the plugin's version pin logs a
mismatch on every run. This is the only source modification.

**3. Configure and build:**

```bat
call "<VS>\VC\Auxiliary\Build\vcvars64.bat"

cmake -S fftw-3.3.11 -B fftw-build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_SHARED_LIBS=ON ^
  -DBUILD_TESTS=OFF ^
  -DENABLE_FLOAT=ON ^
  -DENABLE_LONG_DOUBLE=OFF ^
  -DENABLE_SSE2=ON ^
  -DENABLE_AVX=ON ^
  -DENABLE_AVX2=ON ^
  -DENABLE_THREADS=OFF ^
  -DENABLE_OPENMP=OFF ^
  -DDISABLE_FORTRAN=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5

cmake --build fftw-build
```

* `ENABLE_SSE2`, `ENABLE_AVX` and `ENABLE_AVX2` are three independent upstream options, each `OFF`
  by default; all three must be given.
* `ENABLE_THREADS=OFF` / `ENABLE_OPENMP=OFF` give a DLL exporting **78** `fftwf_*` functions with no
  threading API. The plugin parallelises across channels and never uses FFTW's threading.
* `BUILD_SHARED_LIBS=ON` produces a DLL rather than a static library.

Output: `fftw3f.dll` (and `fftw3f.lib`) in the build directory.

**4. Give the artifacts the project's names.**

1. Copy the DLL to `bin/libfftw3f-3.3.11-avx2.dll`.
2. Produce the `.def` from its export table (`dumpbin /nologo /exports fftw3f.dll`), with first line
   `LIBRARY libfftw3f-3.3.11-avx2.dll`. The vendored `.def` has **125** entries: 78 `fftwf_`
   functions plus 47 legacy `sfftw_` aliases.
3. Save it as `lib/libfftw3f-3.3.11-avx2.def`.
4. Generate the import library:
   `lib.exe /nologo /machine:x64 /def:libfftw3f-3.3.11-avx2.def /out:libfftw3f-3.3.11-avx2.lib`.
   `td_plugin_use_fftw3()` runs this for you when the `.lib` is missing, but into
   `<build>/td_deps/fftw3/`, so run it yourself to keep the vendored tree complete.

The `.def`'s `LIBRARY` line must match the DLL's file name: the import library embeds that name and
the loader follows it, so a mismatch fails with "module not found" while the right DLL sits beside
it. When they disagree, the CMake builds from a corrected copy in the build tree.

**5. Copy the header.** Take upstream `fftw3.h` unmodified as `include/fftw3-3.3.11-avx2.h` and keep
the `include/fftw3.h` redirector pointing at it. Byte-for-byte is what lets its SHA-256 (in
`VERSION`) be checked against the tarball.

**6. Update `VERSION`:** version, release date, build tag, SIMD set, compiler, source URL and MD5,
the DLL's SHA-256 (`certutil -hashfile libfftw3f-3.3.11-avx2.dll SHA256`), export count, and the
`fftwf_version` string.

**7. Verify.** Build the plugin, then run `build/bin/Release/fft_version_probe.exe`. It reports the
DLL actually loaded (full path and size), its version, whether it exports the threading API, the
SIMD codelet family FFTW picks at the default size, and plan-build and execute cost at every
Zero-Pad length the plugin offers. It uses only the vendored header, so it measures the library,
not the plugin.

### Comparing two FFTW builds

`fft_version_probe` is the A/B tool: run it, swap the DLL next to the exe, run it again — no
rebuild. The substitute must keep the same file name, because the loader resolves the import by
name; the probe prints the real `fftwf_version` and full path, so a mislabelled file shows. No
FFTW-vs-FFTW numbers are kept here: they depend on the machine, so re-run the probe to take one.

## Replacing it with a different FFTW build

`ROOT` is this directory, `stem` is `libfftw3f` (or `libfftw3` for double, via `PRECISION d`), and
`tag` is the string passed as `VERSION`:

| path | required? | notes |
|---|---|---|
| `<ROOT>/include/fftw3.h` | yes | the include name every consumer writes; real header or redirector |
| `<ROOT>/include/fftw3-<tag>.h` | optional | used in preference to `fftw3.h` when present |
| `<ROOT>/lib/<stem>-<tag>.def` | optional | needed only if the `.lib` has to be generated (into the build tree) |
| `<ROOT>/lib/<stem>-<tag>.lib` | yes (unless `DYNAMIC`) | the import library. Under `DYNAMIC` the configure fails only if the header is missing |
| `<ROOT>/bin/<stem>-<tag>.dll` | yes in practice | also accepted in `lib/` or `ROOT`. Without it the configure warns and the plugin loads whatever else it finds |
| `<ROOT>/VERSION` | optional | provenance only; feeds four log lines and one CMake target property |

**What the CMake assumes:**

1. **The tag is spelled identically in the header, `.def`, `.lib` and `.dll`.** A mismatch in one
   name means that artifact is not found.
2. **More than one versioned `.lib` under `lib/` is a hard error unless you pass `VERSION`.** Version
   strings do not sort lexicographically (`3.3.6` > `3.3.11` as text), so the CMake refuses to
   guess. With exactly one, the tag is inferred from its name; with none, the unversioned layout
   (`libfftw3f-3.{dll,lib,def}` + `include/fftw3.h`) is assumed.
3. **A wrong `.def` `LIBRARY` line is corrected in the build tree** (`<build>/td_deps/fftw3/`), so a
   `.def` that kept upstream's `LIBRARY fftw3f.dll` still works; the vendored files are never edited.
4. **A `.lib` is generated only when the vendored one is missing or its `.def` names the wrong DLL**
   (MSVC, with `lib.exe` on `PATH`; otherwise the configure warns). A vendored `.lib` whose `.def`
   has the right `LIBRARY` line is used as-is even if the export list changed.
5. **`DYNAMIC` covers the plugin only.** `_td_add_console()` links `TD_PLUGIN_FFTW3_LINK` into
   `fft_tests`, `fft_bench` and the probes and defines `FFTW_DLL` for them, so a broken `.lib` still
   breaks those targets.
6. **`FFTW_DLL` matters for MSVC consumers that link.** Without it the data symbols (`fftwf_version`,
   `fftwf_cc`, `fftwf_codelet_optim`) bind to the import library's jump thunk, and reading
   `fftwf_version` yields machine code. `td_plugin_use_fftw3()` defines it for linked targets.
7. **The plugin's runtime list does not read the tag.** A DLL name not in `kFftw3Dlls` must be added
   there, and a version other than `"3.3.11"` logs `** MISMATCH` (it still runs).

**The symbols the plugin resolves.** `loadBackendFrom()` looks up **twelve** `fftwf_` symbols (the
same list for both backends), plus `MKL_Set_Threading_Layer` on the oneMKL path. Nothing is linked.

| symbol | required? | role | in vendored `.def` |
|---|---|---|---|
| `fftwf_malloc`, `fftwf_free` | **required** | the library's own aligned allocator pair; plan buffers come from here and are released here, never with `free()` | yes |
| `fftwf_plan_dft_r2c_1d`, `fftwf_execute_dft_r2c`, `fftwf_destroy_plan` | **required** | plan, run, destroy. With the allocator pair, these are the five `FftApi::complete()` demands; a library missing any is rejected | yes |
| `fftwf_import_wisdom_from_filename`, `fftwf_export_wisdom_to_filename` | optional | needed together for a working wisdom cache | yes |
| `fftwf_sprint_plan` | optional | the only runtime view of which SIMD kernels were chosen | yes |
| `fftwf_init_threads`, `fftwf_plan_with_nthreads` | optional | resolved for **existence only**, never called; reported as "threads API present (unused)" | **no** (built `-DENABLE_THREADS=OFF`) |
| `fftwf_version`, `fftwf_cc` | optional | **data** symbols, read as C strings | yes |
| `MKL_Set_Threading_Layer` | optional | oneMKL only, when the descriptor asks for it; not an `fftwf_` symbol | — |

The vendored DLL exports ten of the twelve; the two missing threads entry points are optional and
their absence only drops the "threads API present" note. A replacement build needs the five required
symbols, plus the wisdom pair for the wisdom cache. `fftwf_cleanup` and `fftwf_forget_wisdom` are
deliberately not resolved (they free process-global state; `FftBackend.h` explains).

## The CPU requirement is not universal

**This build requires AVX2 and FMA.**

* **AVX2:** the build enables AVX2 codelets, and the DLL's version string is
  `fftw-3.3.11-sse2-avx-avx2-avx2_128`. `VERSION` records `SSE2+AVX+AVX2(FMA)`.
* **FMA:** FFTW's CMake states "AVX2 codelets require FMA support as well", its AVX2 SIMD header calls
  `_mm256_fmadd*` with no `HAVE_FMA` guard, and the shipped DLL contains thousands of FMA3
  instructions (`vfmadd231ps`, `vfmaddsub231ps`, `vfnmadd231ps`). A CPU with AVX2 but no FMA3
  cannot run it.

What that excludes:

* **Intel before Haswell (2013)** — AVX2 and FMA3 arrive together there.
* **AMD before Zen (2017)** — Excavator and earlier have at best AVX, some FMA without AVX2.

**The plugin refuses to run rather than crash.** `FFT.dll` itself is built with `/arch:AVX2`, so the
node checks the CPU at creation (`myCpuOk`): without AVX2 + FMA every cook outputs silence and the
node shows an error, and no FFTW code is ever executed. The DLL would otherwise load and resolve on
any x64 CPU, and its first transform would raise `STATUS_ILLEGAL_INSTRUCTION`. The headless tools
(`fft_tests`, `fft_bench`, the probes) have no such guard and do crash on such a CPU. Run
`build/bin/Release/fft_version_probe.exe` on the machine in question: it prints the DLL that loaded
and the codelet family FFTW selected.

## Where it came from

Built from **https://fftw.org/pub/fftw/fftw-3.3.11.tar.gz** with the recipe
[above](#rebuilding-this-exact-library-from-source). It is built from source because:

* **fftw.org publishes no AVX2 Windows binary.** Its Windows page offers prebuilt 3.3.5 DLLs only;
  3.3.11 is the latest stable release.
* **Upstream's contributed `CMakeLists.txt` carries the wrong version** (`3.3.10`) and needs the
  one-line patch in step 2; the patched block is quoted in `VERSION`.

## Two libraries, one ABI: why nothing is linked

FFTW3 and Intel oneMKL's FFTW3 compatibility interface export the same `fftwf_*` symbols. Linking
one would fix the choice at build time, with no fallback and no way to compare. So neither is
linked: `source/FftBackend.h` resolves the chosen library with `LoadLibraryEx` + `GetProcAddress`
and the engine calls through that table. `dumpbin /dependents FFT.dll` shows only `KERNEL32`, the
MSVC runtime and the `api-ms-win-crt-*` shims — no FFTW, no MKL. Because `GetProcAddress` returns
the real address of `fftwf_version` / `fftwf_cc`, the `FFTW_DLL` data-symbol trap does not apply to
the plugin.

## Using Intel oneMKL instead (the FFT Backend toggle)

The node's **FFT Backend** toggle (Performance page) switches at run time between this FFTW3 build
and Intel oneMKL's FFTW3 interface.

### Performance

`fft_bench`, i9-13900H (AVX2, no AVX-512), median of 3 pinned runs, fft+mag stage:

| N | FFTW3 3.3.11, `FFTW_MEASURE` plan | Intel oneMKL 2026.1 | oneMKL faster by |
|---|---|---|---|
| 8192 | 4.71 µs | 3.87 µs | 18 % |
| 16384 | 11.55 µs | 8.41 µs | 27 % |
| 32768 | 23.65 µs | 17.73 µs | 25 % |

An `FFTW_PATIENT` plan runs 9.70 µs at 16K; oneMKL still leads by ~13 %. FFTW3 stays the default
because it is 3 MB, vendored and wisdom-cached; oneMKL loads 5 DLLs (~177 MB) and spends ~39 ms
initialising once per process. Details: [`CHANGELOG.md`](../../../../CHANGELOG.md), "FFT backend and
plan flags".

**FFTW plan flags.** The out-of-place, input-preserving r2c plan the plugin uses is FFTW's fastest
mode: `FFTW_DESTROY_INPUT` plus re-zeroing the pad each frame is 1–5 % slower, in-place plus
re-zeroing 10–20 % slower. `FFTW_PATIENT` executes 10–15 % faster than `FFTW_MEASURE` at 16K/32K;
**FFT Planner = Patient** plans it in the background once per size with no time limit (~2.7 s at
N = 32768 on the i9-13900H, longer on slower machines) and caches it in wisdom.

**To measure:** `fft_bench --backend fftw3` or `fft_bench --backend mkl`. The flag takes names only;
a number (e.g. `--backend 1`) prints one "unknown --backend" line and runs FFTW3.

### Plan control and the wisdom cache

oneMKL picks its own plan. It accepts `FFTW_MEASURE` / `PATIENT` / `WISDOM_ONLY` and ignores them, and
its wisdom functions return 0 without touching a file. On that backend the engine plans with
`FFTW_ESTIMATE`, **FFT Planner** (Spectrum page) has no effect, and the plan line and Info DAT say the
library picks its own plan. Plan control plus Intel's kernels would need oneMKL's native DFTI
interface (`DftiCreateDescriptor` / `DftiComputeForward`) as a separate engine behind `IFFTEngine`.

### Why the oneMKL backend reports "FFTW 3.3.4"

The `fft_backend` Info DAT row and the log line say `FFTW 3.3.4 wrappers to Intel oneMKL` (the bench
prints it as `backend live:`). The string is `fftwf_version` compiled into `mkl_rt.3.dll`: 3.3.4 is
the FFTW API level Intel's compatibility layer implements, stable across oneMKL releases because the
FFTW3 public ABI has not changed since 3.0. Both sides are current releases: FFTW 3.3.11 and oneMKL
2026.1 (`intelmkl.redist.win-x64` 2026.1.0.226).

`mkl_rt.3.dll` exports **95** `fftwf_*` symbols, the vendored DLL **78**. oneMKL lacks the post-3.3.4
additions (`fftwf_copy_plan`, `fftwf_planner_nthreads`, `fftwf_threads_set_callback`) and adds 23
Intel-only `fftwf_*_omp_offload` entry points. **All twelve symbols the plugin resolves are exported
by `mkl_rt.3.dll`.**

### Deploying oneMKL

oneMKL is not redistributed in this repository: its DLLs are Intel-licensed and the ISSL requires
their notice files to ship with them. To enable the toggle:

1. Get the runtime — `intelmkl.redist.win-x64` from nuget.org, or the oneMKL component of the oneAPI
   toolkit (https://www.intel.com/content/www/us/en/developer/tools/oneapi/onemkl-download.html).
2. Copy these **14 DLLs (~456 MB)**, all the same version, next to `FFT.dll` in
   `Plugin_FFT/__Plugins__/FFT/`:
   `mkl_rt.3.dll`, `mkl_core.3.dll`, `mkl_sequential.3.dll`; the CPU kernel set `mkl_def.3.dll`,
   `mkl_mc3.3.dll`, `mkl_avx2.3.dll`, `mkl_avx512.3.dll`, `mkl_avx10.3.dll`; and the VML set
   `mkl_vml_def.3.dll`, `mkl_vml_mc3.3.dll`, `mkl_vml_avx2.3.dll`, `mkl_vml_avx512.3.dll`,
   `mkl_vml_avx10.3.dll`, `mkl_vml_cmpt.3.dll`.
   * The plugin looks for `mkl_rt.3.dll` (oneMKL 2026.x), then `mkl_rt.2.dll` (2025.x), then
     `mkl_rt.dll` (2020.4 and earlier).
   * On the i9-13900H only five load: `mkl_rt`, `mkl_core`, `mkl_sequential`, `mkl_avx2`,
     `mkl_vml_avx2` (~177 MB). The other kernel/VML variants are there for AVX-512 or older CPUs.
   * **Not needed:** `mkl_intel_thread.3.dll`, `mkl_tbb_thread.3.dll` (the sequential layer is forced,
     see below) and `libimalloc.dll` (never loaded by the FFT path). `libiomp5md.dll` must not be
     added either; the redist package does not contain one.
3. Copy the notices into `__Plugins__/FFT/oneMKL-licenses/`: the package's `license.txt` and
   `third-party-programs.txt` (from `share/doc/mkl/licensing/`). `.gitignore` keeps the whole
   deployment out of the repository.
4. Turn **FFT Backend** on (Performance page). The `fft_backend` Info DAT row and the textport log
   name the library, its path and version. Each plan logs a `[oneMKL] plan N=... in x ms` line; the
   full backend description prints once per backend, then only a short kernel note when the chosen
   kernels change.

The library is opened from the plugin's own directory with `LOAD_WITH_ALTERED_SEARCH_PATH`, so oneMKL
finds its sibling DLLs there with no `PATH` entry. Staging them in `build/bin/Release` serves the
bench and tests only; TouchDesigner needs them in `__Plugins__/FFT/`. If they are missing, the
plugin logs the file it looked for and falls back to FFTW3 for that node.

### Two things this project does deliberately with oneMKL

* **`MKL_Set_Threading_Layer(MKL_THREADING_SEQUENTIAL)` is called as soon as the library loads.**
  oneMKL's default Intel OpenMP layer brings `libiomp5md.dll` into the process, TouchDesigner already
  loads its own, and two Intel OpenMP runtimes in one process abort with *"Error #15: Initializing
  libiomp5md.dll, but found libiomp5md.dll already initialized"*. The ISSL forbids modifying the
  DLLs, so selecting the sequential layer is the plugin's only fix. It also matches the threading
  model: the plugin parallelises across channels, never inside a transform. With the switch, neither
  `mkl_intel_thread.3.dll` nor `libiomp5md.dll` is loaded; `fft_tests` asserts this.
* **Planner policy and wisdom are reported as inapplicable, not silently ignored** — see
  [Plan control and the wisdom cache](#plan-control-and-the-wisdom-cache).

## License

FFTW is GPL, and the vendored DLL is deployed beside `FFT.dll`, so distributing the plugin falls
under the GPL. This directory holds no copy of FFTW's `COPYING`, and the repository has no `LICENSE`
file; adding both is planned in [`INSTALLER_PLAN.md`](../../../../INSTALLER_PLAN.md) §6. oneMKL is
under the Intel Simplified Software License and is not distributed with this project. See
"License / third party" in the [project README](../../../../README.md).
