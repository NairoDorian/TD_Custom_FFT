# FFTW3 for Windows 64-bit (single precision) — vendored 3.3.11, AVX2 + FMA

> **Build-system note:** this vendored tree is consumed through the sibling
> [`PluginBuilder_V2`](../../../../../PluginBuilder_V2) CMake module — specifically
> `td_plugin_use_fftw3(FFT VERSION 3.3.11-avx2 DYNAMIC)` in
> [`PluginProjects/FFT/CMakeLists.txt`](../../CMakeLists.txt). `DYNAMIC` means no import library
> is linked into `FFT.dll`; the DLL is staged next to the plugin and resolved at run time (see
> `source/FftBackend.h`). If you change the build tag, file names, or link mode, update both this
> README and `PluginBuilder_V2/cmake/TDPlugin.cmake`'s `td_plugin_use_fftw3` contract, and rebuild
> via `PluginBuilder.tox` or `python PluginBuilder_V2/dev/ci.py --project PluginProjects/FFT`.

Only the single-precision runtime (`fftwf_` / `libfftw3f`) is vendored. The full record of what
this build is — version, source, hashes, build recipe, the one upstream patch — is in `VERSION`
next to this file; this README is the short version and the install instructions.

## Contents

1. [What this is, in one minute](#what-this-is-in-one-minute) — orientation for a first-time reader
2. [What is in this directory](#what-is-in-this-directory) — the file inventory, and why every name carries a version
3. [How the build finds and uses this](#how-the-build-finds-and-uses-this) — the call chain, end to end
4. [Rebuilding this exact library from source](#rebuilding-this-exact-library-from-source)
5. [Replacing it with a different FFTW build](#replacing-it-with-a-different-fftw-build) — including every assumption the CMake makes
6. [The CPU requirement is not universal](#the-cpu-requirement-is-not-universal) — AVX2 + FMA, and what that excludes
7. [Where it came from](#where-it-came-from) — provenance, and the two things that cost real time
8. [Two libraries, one ABI: why nothing is linked](#two-libraries-one-abi-why-nothing-is-linked)
9. [Using Intel oneMKL instead (the FFT Backend toggle)](#using-intel-onemkl-instead-the-fft-backend-toggle)
10. [License](#license)

## What this is, in one minute

**FFTW is the library that actually computes the FFT.** This plugin does not implement a Fourier
transform itself: it prepares a window, hands a block of floats to FFTW, and reads back the complex
bins. FFTW is a foreign, pre-compiled DLL, and this directory is the copy the plugin loads.

The four things a reader usually needs, up front:

| question | answer |
|---|---|
| What is vendored here? | FFTW **3.3.11**, **single precision only** (`fftwf_` / `libfftw3f`) — the double and long-double builds are not shipped |
| Which exact build? | A locally built **Windows x64** DLL from the upstream tarball, with **SSE2 + AVX + AVX2 codelets**, **threading and OpenMP off** |
| Built with what? | CMake + Ninja + **MSVC 19.51** (VS 18.10.1 Community), Release, `x64` |
| Build tag on disk | **`3.3.11-avx2`**, so every artifact is named `libfftw3f-3.3.11-avx2.*` |
| Where did it come from? | `https://fftw.org/pub/fftw/fftw-3.3.11.tar.gz` (MD5 `40ec8d0447d03b8f01f8c90aa77bd16f`), built locally, not downloaded as a binary — [why](#where-it-came-from) |
| Where does the plugin use it? | `PluginProjects/FFT/CMakeLists.txt` → `td_plugin_use_fftw3(FFT VERSION 3.3.11-avx2 DYNAMIC)`; the DLL is deployed next to `FFT.dll`, and `source/FftBackend.h` opens it **at run time** |
| Is the record complete? | Yes — `VERSION` beside this file is the authoritative record (SHA-256, source MD5, flags, the one patch) |

Two consequences of that table are worth stating before anything else, because they explain almost
every odd-looking decision further down:

* **Nothing links against this library.** `DYNAMIC` in the CMake call above means the plugin
  resolves `fftwf_*` through `LoadLibraryEx` + `GetProcAddress` instead of the linker. That is
  deliberate, and [§Two libraries, one ABI](#two-libraries-one-abi-why-nothing-is-linked) is the
  reason.
* **The build is not universal.** These codelets need AVX2 **and** FMA, which excludes older CPUs.
  [§The CPU requirement is not universal](#the-cpu-requirement-is-not-universal) says exactly what
  that means for the plugin.

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

Two notes on that listing, because earlier revisions of this file got both wrong:

* The **`.def` lives in `lib/`, not in `include/`**. `td_plugin_use_fftw3()` reads it from
  `lib/<stem>-<tag>.def` and writes the import library beside it. Nothing anywhere looks for a
  `.def` under `include/`.
* **There is no `.pdb` here.** A previous revision of this README listed
  `lib/libfftw3f-3.3.11-avx2.pdb`; it is not shipped. A `.pdb` is a debug-symbols file, useful only
  for stepping into FFTW itself in a debugger, and it is not needed to link, load or deploy this
  library — `td_plugin_use_fftw3()` never looks for one, and neither does `FftBackend.h`. The
  `.exp` file above is different: it is written as a side effect of `lib.exe` and is harmless to
  keep beside the `.lib`.

Every file is named with the version and the SIMD level it was built for. That is deliberate: two
FFTW builds that differ only in SIMD produce identical results and identical `fftwf_version`
prefixes, so the file name is the only thing on disk that says which one is in play.

## How the build finds and uses this

`td_plugin_use_fftw3(FFT VERSION 3.3.11-avx2 DYNAMIC)` in `PluginProjects/FFT/CMakeLists.txt`
(defined in `PluginBuilder_V2/cmake/TDPlugin.cmake`) locates these files, generates a `.lib` from
the `.def` (in the build tree, never in this directory) only when the vendored one is missing or
names the wrong DLL, and stages/deploys the DLL. `DYNAMIC` means the plugin does **not**
link the import library — see [§Two libraries, one ABI](#two-libraries-one-abi-why-nothing-is-linked).

The whole path, in the order it happens:

| step | who | what happens |
|---|---|---|
| 1 | `PluginProjects/FFT/CMakeLists.txt` | calls `td_plugin_use_fftw3(FFT VERSION 3.3.11-avx2 DYNAMIC)` |
| 2 | `_td_find_3rdparty()` | finds this directory. It tries, in order: `<plugin source>/3rdParty/fftw3`, then `${TD_SAMPLES_DIR}/3rdParty/fftw3`, then `${PLUGIN_BUILDER_DIR}/3rdParty/fftw3`. The first that exists wins, so this plugin-local copy always wins here |
| 3 | `td_plugin_use_fftw3()` | picks the tag (`3.3.11-avx2`), so the stem `libfftw3f` plus the tag gives `libfftw3f-3.3.11-avx2` |
| 4 | `td_plugin_use_fftw3()` | adds `include/` to the FFT target and defines `TD_PLUGIN_HAS_FFTW3=1` (plus `TD_PLUGIN_FFTW3_DYNAMIC=1` under `DYNAMIC`) |
| 5 | `td_plugin_use_fftw3()` | uses `lib/<stem>-<tag>.lib` as-is when it exists and the `.def`'s `LIBRARY` line names `<stem>-<tag>.dll`; otherwise (MSVC only) writes a corrected `.def` and runs `lib.exe` into `<build>/td_deps/fftw3/`. Nothing is ever written into this directory |
| 6 | `td_plugin_add_runtime_dll()` | copies `bin/<stem>-<tag>.dll` next to `FFT.dll` in the build output and records it in `<build>/FFT_runtime_dlls.txt`. A standalone build also copies it into the deployment folder `Plugin_FFT/__Plugins__/FFT/`; under `PluginBuilder.tox` (`PLUGINBUILDER_BUILD` set) PluginBuilder stages exactly the files that list names |
| 7 | `source/FftBackend.h`, at run time | `loadBackendFrom()` opens the DLL from the plugin's own directory and fills in an `FftApi` table of function pointers |

Step 7 is the part that surprises people, so it is worth naming the functions correctly. There is no
`loadBackendFor()` — no such function exists in this repository. The real names, and the real order,
are:

```
the Backend parameter is an int
  -> backendById()        picks one of the two FftBackendInfo descriptors
  -> cachedBackend()      returns the process-wide FftBackend for it, loading it the first time
  -> loadBackend()        walks the descriptor's dlls[] list, one file name at a time:
                          libfftw3f-3.3.11-avx2.dll, then libfftw3f-3.dll, then fftw3f.dll
  -> loadBackendFrom()    one LoadLibraryExA plus a GetProcAddress per symbol, filling in an FftApi
  -> the FftApi table     every engine call afterwards goes through this table and never names a
                          library symbol directly
```

`loadBackend()` takes the directory to search as a defaulted argument; an omitted or empty one means
`pluginDirectory()`, which is the folder `FFT.dll` itself lives in. That is why the deployed DLL has
to sit beside `FFT.dll` and not in `build/bin`.

The DLL names in the `loadBackend()` step above are an ordered preference list, not a single fixed
name: the first one that opens wins. Two consequences a novice should know:

* **The plugin finds a differently named DLL only if its name is in that list.** Adding an FFTW
  build that keeps its own file name means adding that name to `kFftw3Dlls` in
  `source/FftBackend.h` — the CMake side is happy with any name it was told, but the loader side has
  its own list.
* **The version is pinned for reporting, not for loading.** `kFftw3Backend` sets
  `expectedVersion = "3.3.11"`. A library that reports anything else still runs; `describeBackend()`
  appends a `** MISMATCH` warning to the log line and the Info DAT, so a swap that changes the
  timings cannot pass unnoticed.

## Rebuilding this exact library from source

The recipe below is the one that produced the vendored binary. It is recorded in short form in
`VERSION`; this is the same thing with the steps spelled out. You need Visual Studio (with the
`x64` toolchain), CMake and Ninja, and a shell where `vcvars64.bat` has been run. FFTW's own build
does not need this plugin, and this plugin is not involved in building FFTW.

**1. Get and unpack the source.**

```
https://fftw.org/pub/fftw/fftw-3.3.11.tar.gz
```

Check the MD5 is `40ec8d0447d03b8f01f8c90aa77bd16f` (recorded in `VERSION`) before building anything.

**2. Patch one line — required, not optional.**

Upstream's contributed `CMakeLists.txt` still says

```cmake
set (FFTW_VERSION 3.3.10)
```

It must say `3.3.11`. This is not cosmetic: that value is substituted into `config.h` as
`PACKAGE_VERSION`, which is the string `fftwf_version` returns *and* the version header FFTW stamps
into its wisdom files. Left alone, a genuine 3.3.11 build identifies itself as 3.3.10 forever, and
the plugin's own version pin logs a mismatch on every run. It is the only source modification made
to the vendored build. [§Where it came from](#where-it-came-from) has the longer story.

**3. Configure and build.** These are the flags that produced this DLL, and each one matters:

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

* `ENABLE_SSE2`, `ENABLE_AVX` and `ENABLE_AVX2` are **three independent options** upstream, each
  `OFF` by default; all three have to be given. (An earlier revision of this README said that
  `-DENABLE_AVX2=ON` "also enables SSE2 and AVX". It does not — upstream's CMake checks each flag
  separately and applies it to its own set of source files.)
* `ENABLE_THREADS=OFF` / `ENABLE_OPENMP=OFF` are what make the resulting DLL export **78**
  `fftwf_*` functions with **no threading API at all**. The plugin never uses FFTW's threading — it
  parallelises across channels — so this is a smaller surface for no loss.
* `BUILD_SHARED_LIBS=ON` is what produces a DLL rather than a static library.

Output: `fftw3f.dll` (and `fftw3f.lib`) in the build directory.

**4. Give the artifact the project's names.** The build emits `fftw3f.dll`; this directory expects
`libfftw3f-3.3.11-avx2.dll`. A plain file rename is not enough for the *import library*, which is
the one step people get wrong:

1. Copy the DLL to `bin/libfftw3f-3.3.11-avx2.dll`.
2. Produce the `.def` from the built DLL's export table:
   `dumpbin /nologo /exports fftw3f.dll`, then turn that into a `.def` whose first line is
   `LIBRARY libfftw3f-3.3.11-avx2.dll` and whose `EXPORTS` section lists the names. The vendored
   `.def` has **125** entries: 78 `fftwf_` public functions plus 47 legacy `sfftw_` aliases.
3. Save it as `lib/libfftw3f-3.3.11-avx2.def`.
4. Generate the import library from it:
   `lib.exe /nologo /machine:x64 /def:libfftw3f-3.3.11-avx2.def /out:libfftw3f-3.3.11-avx2.lib`.
   You can also skip this — `td_plugin_use_fftw3()` runs exactly this command for you when the
   `.lib` is missing, but it writes the result into `<build>/td_deps/fftw3/`, not into `lib/`, so
   the vendored tree is only complete on its own if you run it yourself.

The reason the `.def`'s `LIBRARY` line must name the same file the DLL is called: the import library
embeds that name, and the loader follows the **embedded** name, not the `.lib`'s own file name. So a
renamed-dll-but-untouched-def pair fails at load time with "module not found" while the correctly
named DLL sits right beside it. When the two disagree the CMake builds from a corrected copy of the
`.def` (in the build tree), so a future re-versioning cannot silently produce that mismatch.

**5. Copy the header.** Take the upstream `fftw3.h` unmodified, name it
`include/fftw3-3.3.11-avx2.h`, and leave the small `include/fftw3.h` redirector pointing at it.
Keeping the upstream file byte-for-byte is what lets its hash be checked against the tarball at any
time; the recorded SHA-256 of the vendored copy is in `VERSION`.

**6. Update `VERSION`.** It is the record: version, release date, build tag, SIMD set, compiler,
source URL, source MD5, the DLL's SHA-256, the export count, and the version string `fftwf_version`
reports. Compute the DLL hash with `certutil -hashfile libfftw3f-3.3.11-avx2.dll SHA256`.

**7. Verify before trusting it.** Configure and build the plugin, which prints the resolved paths,
then run the probe:

```
build/bin/Release/fft_version_probe.exe
```

It reports the DLL this process actually loaded (full path and size), the version that DLL reports,
whether it exports the threading API, which SIMD codelet family FFTW picks for the plugin's default
size, and the plan-build and execute cost of one real-to-complex transform at every Zero-Pad length
the plugin offers. It includes the vendored header and nothing from `source/`, so it measures the
library itself rather than the plugin's use of it.

### Comparing two FFTW builds

`fft_version_probe` is also the A/B tool: put one DLL next to the exe, run it, keep the output, swap
in the other DLL, run it again. The exe is not rebuilt between runs — only the file on disk changes.
The substitute must carry the **same file name** as the one the exe was linked against, because the
module loader resolves the import by name; the probe prints the real `fftwf_version` and the
resolved full path, so a mislabelled file cannot pass unnoticed. (No FFTW-vs-FFTW numbers are kept in
this README — the CMake comment that introduces the probe says the same. The probe prints them per
machine, and they move with the CPU and with FFTW's own version.)

## Replacing it with a different FFTW build

Everything here is a fixed contract, not a convention. Dropping in a different build is four files
with matching names plus one optional record — but the CMake makes assumptions that are not all
obvious, so they are listed explicitly.

**The directory layout `td_plugin_use_fftw3()` requires.** `ROOT` is this directory
(`3rdParty/fftw3`), `stem` is `libfftw3f` for single precision (`libfftw3` for double, selected with
`PRECISION d`), and `tag` is whatever version string you pass as `VERSION`:

| path | required? | notes |
|---|---|---|
| `<ROOT>/include/fftw3.h` | yes | the include name every consumer writes. Can be the real header or a redirector |
| `<ROOT>/include/fftw3-<tag>.h` | optional | used in preference to `fftw3.h` when it exists; this is how the header gets version-stamped |
| `<ROOT>/lib/<stem>-<tag>.def` | optional | needed only if the `.lib` has to be generated (it is then generated into the build tree) |
| `<ROOT>/lib/<stem>-<tag>.lib` | yes (unless `DYNAMIC`) | the import library. Under `DYNAMIC` the configure fails only if the *header* is missing |
| `<ROOT>/bin/<stem>-<tag>.dll` | yes in practice | also accepted in `lib/` or in `ROOT` itself. Without it the configure warns, and the plugin then loads whatever else it can find |
| `<ROOT>/VERSION` | optional | provenance only; feeds four log lines and one CMake target property |

**The assumptions the CMake makes, stated plainly:**

1. **The names are fixed and must agree with each other.** The stem comes from `PRECISION`, but the
   `tag` is whatever `VERSION` was passed, and it must be spelled identically in the header, the
   `.def`, the `.lib` and the `.dll`. `3.3.11-avx2` is used here; a mismatch in any one of the four
   names means that artifact is simply not found.
2. **More than one versioned `.lib` under `lib/` is a hard error unless you pass `VERSION`.** The
   CMake refuses to guess, deliberately: version strings do not sort lexicographically (`3.3.6` >
   `3.3.11` as text), and silently linking the wrong FFTW is exactly the mistake the tag exists to
   prevent. With exactly one, the tag is inferred from its file name; with none, the legacy
   unversioned layout (`libfftw3f-3.{dll,lib,def}` + `include/fftw3.h`) is assumed.
3. **The `.def`'s `LIBRARY` line is corrected for you** when it disagrees with the DLL name, so a
   copy that kept upstream's `LIBRARY fftw3f.dll` line still works. The vendored file is not edited:
   the corrected `.def` and the `.lib` built from it go to `<build>/td_deps/fftw3/`. (Earlier
   revisions rewrote the files here whenever the `.def` was not older than the `.lib`, which after
   every git checkout left the vendored `.lib` permanently "modified".)
4. **A `.lib` is generated only when the vendored one is missing or its `.def` names the wrong
   DLL** — not on timestamps. This only happens under MSVC and only if `lib.exe` can be found on
   `PATH`; otherwise the configure warns and uses whatever `.lib` is there. A vendored `.lib` whose
   `.def` has the right `LIBRARY` line is used as-is even if the export list changed, which is why a
   `.lib` that does not match its `.def` is worth checking by hand.
5. **`DYNAMIC` does not mean "nothing links this".** The plugin does not link it, but the headless
   executables do: `_td_add_console()` links `TD_PLUGIN_FFTW3_LINK` into `fft_tests`,
   `fft_bench` and the probes, and defines `FFTW_DLL` for them, because they are single-backend by
   construction and read `fftwf_version` directly. So a broken `.lib` still breaks those targets
   even though the plugin itself never uses it.
6. **`FFTW_DLL` matters for MSVC consumers.** `fftw3.h` says so itself ("for Windows compilers, you
   should add ... `#define FFTW_DLL`"). Without it, data symbols (`fftwf_version`, `fftwf_cc`,
   `fftwf_codelet_optim`) bind to the import library's jump thunk rather than to the data in the
   DLL, so reading `fftwf_version` as a C string yields machine code instead of a version string.
   `td_plugin_use_fftw3()` defines it for linked targets; MinGW/libtool does it for you, which is
   why the trap is so often forgotten.
7. **The plugin's runtime search list is separate and does not read the tag.** `kFftw3Dlls` in
   `source/FftBackend.h` names the files the loader will try. A replacement DLL with a name that is
   not in that list has to be added there, and `expectedVersion` there is pinned to `"3.3.11"`, so a
   different version logs a `** MISMATCH` warning (it still runs).

**The symbol set the plugin actually needs.** All of it is resolved at run time; nothing is linked.
Thirteen `fftwf_` symbols are looked up (the same list for both backends), plus one non-FFTW symbol
on the oneMKL path:

| symbol | required? | role |
|---|---|---|
| `fftwf_malloc`, `fftwf_free` | **required** | the library's own aligned allocator pair — buffers handed to a plan must come from here and be released here, never with `free()` |
| `fftwf_plan_dft_r2c_1d`, `fftwf_execute_dft_r2c`, `fftwf_destroy_plan` | **required** | plan, run, destroy. Together with the allocator pair these are the five `FftApi::complete()` demands, and a library missing any of them is rejected |
| `fftwf_import_wisdom_from_filename`, `fftwf_export_wisdom_to_filename` | optional | required *together* to count as a working wisdom cache |
| `fftwf_sprint_plan` | optional | the only runtime way to see which SIMD kernels the library chose |
| `fftwf_init_threads`, `fftwf_plan_with_nthreads` | optional | resolved for their **existence only**, never called. Their presence is reported as "threads API present (unused)" |
| `fftwf_version`, `fftwf_cc` | optional | **data** symbols, read as C strings |
| `MKL_Set_Threading_Layer` | optional | oneMKL only, and only when the descriptor asks for it. Not an `fftwf_` symbol at all |

So a replacement build needs the five required symbols present, and the wisdom pair if you want the
wisdom cache to work. A `-DENABLE_THREADS=OFF` build like this one is perfectly usable — see
[§Two libraries, one ABI](#two-libraries-one-abi-why-nothing-is-linked) and the oneMKL section for
what that does and does not change.

## The CPU requirement is not universal

**This build requires AVX2 and FMA, and neither is universal.** Two independent facts make it true,
and both are checkable against the files shipped here rather than taken on trust:

* **AVX2.** The build enables AVX2 codelets (`-DENABLE_AVX2=ON`), and the library names itself
  accordingly: the version string inside `libfftw3f-3.3.11-avx2.dll` is
  `fftw-3.3.11-sse2-avx-avx2-avx2_128`. `VERSION` records `SSE2+AVX+AVX2(FMA)` and
  `config.h: HAVE_SSE2=1 HAVE_AVX=1 HAVE_AVX2=1`.
* **FMA as well, not just AVX2.** FFTW's own CMake says it outright — "AVX2 codelets require FMA
  support as well" — and its AVX2 SIMD header calls `_mm256_fmadd*` unconditionally, with no
  `HAVE_FMA` guard around the transform kernels. Disassembling the shipped DLL confirms it: the
  binary contains thousands of FMA3 instructions (`vfmadd231ps`, `vfmaddsub231ps`, `vfnmadd231ps`).
  A CPU without FMA3 cannot run this DLL even if it has AVX2.

Practically, that means:

* **Pre-2013 Intel CPUs are excluded.** AVX2 and FMA3 arrive together on Intel's Haswell (2013) and
  later. Ivy Bridge (2012) and everything older has neither.
* **Most AMD CPUs before Zen are excluded.** AMD shipped AVX2 + FMA3 from Zen (2017) onward;
  Excavator and earlier have at best AVX, in some cases FMA without AVX2, and older parts have
  neither.
* **What it means for the plugin:** the failure is not a clean one. Windows' loader does not execute
  a DLL's code, so this library **loads** fine on a CPU without AVX2/FMA and the plugin's symbol
  resolution succeeds — nothing checks the CPU at startup, because FFTW's build has no runtime
  dispatch for a codelet set it was compiled to use unconditionally. The fault lands on the **first
  transform**: executing an AVX2/FMA codelet on a CPU that lacks it raises an illegal-instruction
  exception (`STATUS_ILLEGAL_INSTRUCTION`), which terminates the host process rather than returning
  an error the plugin could catch and fall back from. In TouchDesigner that is a crash, not a
  degraded node. The FFT Backend toggle is not a way out either: Intel oneMKL also loads
  architecture-specific kernel DLLs at run time, so a machine too old for AVX2 is too old for the
  accelerated FFT paths generally. The only safe fallback is the unversioned `libfftw3f-3.dll` entry
  in the search list — fftw.org's prebuilt Windows DLL is 3.3.5/SSE2 and has no such requirement —
  but that file is not part of this repository and nothing here will fetch it for you.

Running `build/bin/Release/fft_version_probe.exe` on the machine in question is the cheap way to
settle it: it prints the DLL that actually loaded and the SIMD codelet family FFTW selected, so a
silent fallback is visible rather than assumed.

## Where it came from

Source: **https://fftw.org/pub/fftw/fftw-3.3.11.tar.gz** (MD5 `40ec8d0447d03b8f01f8c90aa77bd16f`),
built locally with CMake + Ninja + MSVC 19.51, `-DENABLE_SSE2=ON -DENABLE_AVX=ON -DENABLE_AVX2=ON`
(each is a separate upstream option — see the rebuild section),
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

Both of those were re-checked against fftw.org while this revision was written: 3.3.11 is still the
latest stable release listed, and the Windows page still offers only the 3.3.5 prebuilt DLLs, with
no AVX2 among them. (The page does not state the published DLLs' SSE2/AVX flags explicitly; that
detail comes from the known 3.3.5 Windows build configuration, not from a line on the page.)

## Two libraries, one ABI: why nothing is linked

FFTW3 and Intel oneMKL's FFTW3 compatibility interface export the *same* `fftwf_*` symbols. Only one
of them can be linked into a binary, and the choice would then be frozen at build time — no fallback
when one is missing, no way to compare them, no way to let the user pick. So **neither is linked**:
`source/FftBackend.h` resolves the chosen library with `LoadLibraryEx` + `GetProcAddress` at run
time and the engine calls through that table. `dumpbin /dependents FFT.dll` shows no FFT library
among its imports, which is the check that this really is dynamic. (Verified again for this
revision: the deployed `FFT.dll` imports only `KERNEL32`, the MSVC runtime and the CRT
`api-ms-win-crt-*` shims — no FFTW, no MKL.)

That also retires the classic MSVC trap this tree used to document: with a static import,
`fftwf_version` and `fftwf_cc` are *data* and bind to the import library's jump thunk unless
`FFTW_DLL` is defined, so reading them prints machine code. `GetProcAddress` returns the address of
the real array, so the question cannot arise.

## Using Intel oneMKL instead (the FFT Backend toggle)

The node's **FFT Backend** toggle (Performance page) switches between this vendored FFTW3 build and
Intel oneMKL's FFTW3 interface, at run time, on the same node. Measured for v2.9.0 (2026-09-21) on
this project's bench on an i9-13900H (6 visible cores, AVX2, no AVX-512), same binary,
`--fft 16384 --bins 16384`, only the library differing:

| library | fft+mag | total per cook |
|---|---|---|
| FFTW3 3.3.11 AVX2, `FFTW_MEASURE` plan from wisdom | 11.94 µs | 21.06 µs |
| Intel oneMKL 2026.1.0 | 8.85 µs | 17.63 µs |

Four paired runs each way put oneMKL **13–34 % faster on the fft+mag stage** every time
(absolute times move with laptop thermals; the direction did not). A re-measurement on 2026-09-23,
median of 3 pinned runs across three sizes, agrees:

| N | FFTW3, `FFTW_MEASURE` plan | Intel oneMKL | oneMKL faster by |
|---|---|---|---|
| 8192 | 4.71 µs | 3.87 µs | 18 % |
| 16384 | 11.55 µs | 8.41 µs | 27 % |
| 32768 | 23.65 µs | 17.73 µs | 25 % |

An `FFTW_PATIENT` plan narrows the gap but does not close it: 9.70 µs at 16K, so oneMKL still leads
by ~13 %. oneMKL is the faster of the two on this Intel CPU, and it is the reason the toggle exists.
FFTW3 stays the default because it is 3 MB, vendored, and wisdom-cached; oneMKL loads 5 DLLs
(~177 MB) and spends ~39 ms initialising once per process. Both sets of numbers are recorded in the
project [`CHANGELOG.md`](../../../../CHANGELOG.md) (the 2026-09-23 set under "FFT backend and plan
flags"); they are not re-derived here.

**To repeat the A/B, pass the backend by name:** `fft_bench --backend mkl` or `--backend fftw3`. A
number is not accepted — `--backend 1` prints one "unknown --backend" line and runs FFTW3, so both
halves of the comparison measure the same library. An earlier 2026-09-23 comparison made exactly that
mistake and briefly concluded the opposite.

The FFTW3 plan itself was checked the same day, and the current setup is already the fastest
execute: an out-of-place plan that preserves its input (the r2c default). `FFTW_DESTROY_INPUT` plus
re-zeroing the pad every frame is 1–5 % slower, and in-place plus re-zeroing is 10–20 % slower.
`FFTW_PATIENT` executes 10–15 % faster than `FFTW_MEASURE` at 16K/32K; that is what **FFT Planner =
Patient** buys, at a one-time background plan per size (~2.7 s at N = 32768 on the i9-13900H, no time limit
since v2.12.1, so slower machines simply take longer) that is then cached in wisdom.

### Why the oneMKL backend reports "FFTW 3.3.4", and why that is not out of date

The `fft_backend` Info DAT row and the plugin's log line say `FFTW 3.3.4 wrappers to Intel oneMKL`.
The headless bench prints the same string behind the label `backend live:`, which is where that
label comes from — it is the bench's wording, not the plugin's. The version is read from
`fftwf_version` inside `mkl_rt.3.dll`; it is a string compiled into that binary. It is **not** a
version of FFTW that Intel has failed to keep up with, because there is no newer one to move to:

* FFTW's newest release is **3.3.11, Apr 21 2026** (it added `fftwf_copy_plan`, SVE and LoongArch
  SIMD). That is the release vendored here, so the FFTW3 side is current.
* oneMKL's newest release is **2026.1, Jul 7 2026**; `intelmkl.redist.win-x64` 2026.1.0.226 is the
  newest package on nuget.org. That is the runtime installed here, so the oneMKL side is current too.
* 3.3.4 is the FFTW release whose API Intel's compatibility layer implements, and Intel has never
  rebumped it across 30+ oneMKL releases, because the FFTW3 public ABI has been stable since 3.0.
  Intel's own documentation never claims a 3.3.x number at all — the Developer Guide says only that
  the interfaces "correspond to the FFTW versions 2.x and 3.x".

It is a real statement about the API surface, not just a stale string. Dumping exports from the two
installed DLLs: `mkl_rt.3.dll` exports **95** `fftwf_*` symbols and `libfftw3f-3.3.11-avx2.dll`
exports **78**, but oneMKL lacks every post-3.3.4 addition — `fftwf_copy_plan` (3.3.11),
`fftwf_planner_nthreads` and `fftwf_threads_set_callback` (3.3.9) — while adding **23** Intel-only
`fftwf_*_omp_offload` entry points for GPU offload. The kernels behind the wrapper are current:
oneMKL 2026.0's release notes list DFT optimizations for power-of-two 1-D complex transforms, which
is exactly this node's workload.

**Nothing the plugin uses is missing.** The plugin resolves **twelve** `fftwf_` symbols at run
time, plus one non-FFTW symbol on the oneMKL path (`MKL_Set_Threading_Layer`). All twelve are
exported by `mkl_rt.3.dll`. (v2.10–v2.12 also resolved a thirteenth, `fftwf_set_timelimit`, to cap
the background PATIENT measurement at 1.5 s; v2.12.1 removed the cap and the symbol with it.) Ten of
the twelve are exported by the vendored `libfftw3f-3.3.11-avx2.dll`; the
two exceptions are `fftwf_init_threads` and `fftwf_plan_with_nthreads`, which that build does not
export because it was configured `-DENABLE_THREADS=OFF`. Both are **optional** — they are resolved
for their existence only and never called — so their absence costs nothing and cannot produce a null
function pointer either way; `describeBackend()` simply omits its "threads API present (unused)"
note on the FFTW3 side. The five *required* symbols (`fftwf_malloc`, `fftwf_free`,
`fftwf_plan_dft_r2c_1d`, `fftwf_execute_dft_r2c`, `fftwf_destroy_plan`) are exported by both
libraries, which is the property that actually matters.

(An earlier revision of this file said "all 19 real functions it resolves are exported by both
DLLs". The count was wrong — it was twelve at the time, not nineteen — and the blanket "by both" was wrong for
the two threads entry points. The conclusion it was drawn for still holds, for the five required
symbols.) The three symbols that appear only as type names (`fftwf_complex`, `fftwf_plan`,
`fftwf_wisdom`) are typedefs in the header and are correctly exported by neither. `fftwf_cleanup`
and `fftwf_forget_wisdom` are exported by both but appear only in a comment in `FftBackend.h` —
deliberately, for the process-global reason given there.

What the MKL backend *does* cost is plan control: oneMKL picks its own plan, so `FFT Planner`
(Auto / Fast / Measured / Patient, on the Spectrum page) has no effect and `FFTW_WISDOM_ONLY` and the wisdom file are no-ops.
The log line says so rather than leaving it to be discovered by A/B. The only
way to get plan control *and* Intel's kernels would be oneMKL's native DFTI interface
(`DftiCreateDescriptor` / `DftiComputeForward`), which is a separate engine behind `IFFTEngine` and a
much larger change than the toggle — worth it only if the plan policy turns out to matter here more
than the 18–27 % transform win does.

**oneMKL is not redistributed here.** The `intelmkl.redist.win-x64` 2026.1.0.226 package (165.9 MB
download) extracts to **17** runtime DLLs, **521 MB** (16 `mkl_*.dll` plus `libimalloc.dll`); the
14 this plugin can use come to **456 MB**. They are Intel-licensed binaries and the ISSL requires
their notice files to ship with them, which is the user's call to make, not a build script's. To
enable the toggle on a machine:

1. Get the runtime DLLs — `intelmkl.redist.win-x64` from nuget.org, or the oneMKL component of the
   oneAPI toolkit (https://www.intel.com/content/www/us/en/developer/tools/oneapi/onemkl-download.html).
2. Copy these next to `FFT.dll` (into `Plugin_FFT/__Plugins__/FFT/`), all the same version — **14 files, 456 MB**:
   `mkl_rt.3.dll`, `mkl_core.3.dll`, `mkl_sequential.3.dll`, the CPU kernel set `mkl_def.3.dll`,
   `mkl_mc3.3.dll`, `mkl_avx2.3.dll`, `mkl_avx512.3.dll`, `mkl_avx10.3.dll`, and the matching VML set
   `mkl_vml_def.3.dll`, `mkl_vml_mc3.3.dll`, `mkl_vml_avx2.3.dll`, `mkl_vml_avx512.3.dll`,
   `mkl_vml_avx10.3.dll`, `mkl_vml_cmpt.3.dll`.
   *(The `.3` suffix is the ABI version oneMKL 2026.x uses; 2025.x is `.2`, 2020.4 and earlier are
   unversioned. The plugin looks for `mkl_rt.3.dll`, then `mkl_rt.2.dll`, then `mkl_rt.dll`.)*
   **Only the `mkl_rt` name matters for loading; the kernel and VML sets are shipped so that the code
   path Intel dispatches to exists on whatever CPU the project is opened on.** Measured on the
   i9-13900H (AVX2, no AVX-512) with the backend live, the process loads exactly five of them:
   `mkl_rt`, `mkl_core`, `mkl_sequential`, `mkl_avx2`, `mkl_vml_avx2` (~177 MB). An AVX-512 or older
   CPU picks the `avx512`/`avx10` or `def`/`mc3` variants instead, which is why those stay.
   **Not needed, and removed from the deployment (2026-09-23):** `mkl_intel_thread.3.dll` and
   `mkl_tbb_thread.3.dll` (the plugin forces the sequential layer at load, see below, so neither can
   ever be loaded; the TBB one would also need a `tbb12.dll` that is not shipped) and
   `libimalloc.dll` (never loaded by the FFT path). The redist package contains all 17; copying the
   three extra files is harmless but wastes ~66 MB.
   **`libiomp5md.dll` is not on this list and the redist does not ship one** — see the two
   deliberate decisions below. This list used to name it, which was wrong twice over: it is not in
   the package, and asking for it would have produced the Error #15 described below on purpose.
3. Copy the notices too — the package's `license.txt` and `share/doc/mkl/licensing/`, which
   require attribution (`third-party-programs.txt` is the one that matters here). The ISSL permits
   redistribution but requires these to travel with the DLLs. On this machine they live in
   `__Plugins__/FFT/oneMKL-licenses/` (`license.txt` and `third-party-programs.txt`) beside the
   DLLs, and `.gitignore` keeps the whole deployment out of the repository.
4. Open the node's Performance page and turn **FFT Backend** on. The Info DAT row `fft_backend` and
   the textport log name the library, its path and its version, so there is no doubt which one ran.
   In the textport (v2.12), every plan still gets its own `[oneMKL] plan N=... in x ms` line, but the
   long backend description is printed once per backend, and after that only a short kernel note,
   and only when the chosen kernels change — so a resize does not repeat the whole description.

Where the plugin looks for them is **its own directory, not `build/bin`**: `loadBackend()`'s
directory argument defaults to `pluginDirectory()`, and the library is opened with
`LOAD_WITH_ALTERED_SEARCH_PATH` so oneMKL finds its sibling DLLs in that same folder without any
`PATH` entry. Staging them in `build/bin/Release` (what the bench and the tests resolve against)
therefore proves the backend works but does **not** make it work in TouchDesigner — that needs the
copy into `__Plugins__/FFT/`.

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

## License

FFTW is GPL, and the vendored DLL is deployed beside `FFT.dll`, so distributing the plugin falls
under the GPL. Note what is **not** here yet: this directory holds no copy of FFTW's `COPYING`, and
the repository has no `LICENSE` file of its own. Adding both — a `LICENSE` for Plugin_FFT, and
FFTW's `COPYING` shipped with `libfftw3f-3.3.11-avx2.dll` — is planned in
[`INSTALLER_PLAN_2026-09-23.md`](../../../../INSTALLER_PLAN_2026-09-23.md) §6. oneMKL is under the
Intel Simplified Software License and is not distributed with this project. See §"License / third
party" in the [project README](../../../../README.md).
