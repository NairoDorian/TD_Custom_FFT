# TouchDesigner Custom FFT CHOP (`Plugin_FFT`)

A real-time C++ CHOP for **Derivative TouchDesigner** (C++ API 10, TouchDesigner 2025.30000+) that turns
audio into a psychoacoustically scaled magnitude spectrum: FFTW3 (or, optionally, Intel oneMKL)
single-precision R2C transform, 256-bit AVX2/FMA post-processing, Log/Mel/ERB/Bark/Chroma re-mapping, equal-loudness weighting,
dB conversion with selectable reference, and attack/release ballistics — all with zero allocations
in the cook loop.

Built with [PluginBuilder_V2](../PluginBuilder_V2) (hot reload from TouchDesigner) but also builds
standalone with CMake + Ninja and ships headless tests and a per-stage benchmark.

> **Relation to PluginBuilder_V2:** this project is the *consumer*; `../PluginBuilder_V2` is the
> *toolchain*. Shared contract: `cmake/TDPlugin.cmake` (`td_add_plugin`, `td_plugin_use_fftw3`,
> `td_plugin_optimize`), `plugin.json` manifest (family CHOP, optype `Fftcustom`), vendored API-10
> headers in `../PluginBuilder_V2/include/`, rename-in-place deploy into `__Plugins__/FFT/`, and the
> `PluginBuilder.tox` hot-reload loop. Cross-repo CI from the builder folder:
> `python dev/ci.py --project ../Plugin_FFT/PluginProjects/FFT`. Builder-side history lives in
> `../PluginBuilder_V2/CHANGELOG.md` and `../PluginBuilder_V2/AUDIT.md` (§6).

---

## How to use this documentation

Six markdown files, six different jobs. Read the one that matches your question rather than reading
them in order:

| File | What it is for | Read it when |
|---|---|---|
| **README.md** (this file) | The user-facing description: what the node does, every parameter, how to build it, the performance numbers, and the debugging history of the middle-click popup | You are using the node, or you are about to change something and want to know what it is for |
| [CHANGELOG.md](CHANGELOG.md) | Version history, newest first, one entry per release, with the A/B measurements behind each performance change (v2.12.0 has the current per-stage numbers and the list of optimisations that were measured and *not* kept) | You want to know *when* something changed, which build introduced a behaviour, or what a kernel change actually bought |
| [AUDIT_AND_PLAN_2026-09-23.md](AUDIT_AND_PLAN_2026-09-23.md) | The 2026-09-23 audit and phased plan: where the time goes, what has shipped since, what is still open (Phase 5) and why the rejected ideas were rejected | You want the current to-do list and its acceptance criteria, or you are looking for something to make faster |
| [ESSENTIATD_LESSONS_FOR_PLUGIN_FFT_2026-09-23.md](ESSENTIATD_LESSONS_FOR_PLUGIN_FFT_2026-09-23.md) | What is worth porting from EssentiaTD (features, cook-model gaps, test harness), with corrections to the older comparison doc | You are adding a feature or a robustness fix and want prior art |
| [INSTALLER_PLAN_2026-09-23.md](INSTALLER_PLAN_2026-09-23.md) | Plan for a per-user Windows installer: exact DLL set, prerequisite checks, licensing, build pipeline | You want to ship the plugin to another machine |
| [PluginProjects/FFT/3rdParty/fftw3/README.md](PluginProjects/FFT/3rdParty/fftw3/README.md) | The vendored FFTW build, the oneMKL alternative, and the license terms of both | You are rebuilding or replacing the FFT library |

Two conventions used throughout, so the numbers can be trusted:

- **Every performance figure names the machine it was measured on.** Times move with thermals and with
  the FFT plan that happens to be live, so an absolute number without a machine on it means nothing
  here; the *direction* of a comparison is the durable part. Machine names are used consistently:
  "i7-class desktop" is the original development machine, "i9-13900H" is the current one.
- **Claims that could not be verified are labelled as such.** The section on the popup has a
  "What is *not* established" list written for exactly this reason. Please keep that habit when you
  add anything: say whether a statement is measured, derived, or a guess.

---

## Where to change what

A map for the first modification, so you do not have to read the whole tree to find one thing. Every
path is relative to `PluginProjects/FFT/`.

| If you want to change... | Edit |
|---|---|
| the DSP maths (window, FFT call, magnitude, warp, weighting, dB, ballistics, FIFO, EQ, the FFTW engine) | `source/DSPModules.h` - one header, sections are numbered and each has a banner |
| the order the stages run in, or the caching of the window / warp / weighting tables | `source/AnalysisPipeline.h` and `source/AnalysisPipeline.cpp` |
| what a parameter *means* (its range, its default, its menu entries, how it is read), or what is greyed out when | `source/Parameters.h` (definitions) and `source/Parameters.cpp` (`setup()`, the one `eval()` that reads them all, and `setEnableStates()`) |
| the sample-rate / window-length / FFT-size / output-bin-count / axis arithmetic, and what a Quality Preset overrides (`applyPreset`) | `source/RateModel.h` - TouchDesigner-free and shared by the node, the bench and the tests, so all three agree by construction |
| the async worker: the wait-free job / result handoff, Worker Wake, Worker Priority | `source/AsyncAnalysis.h` and `source/AsyncAnalysis.cpp` |
| the TouchDesigner side: the cook, the Info CHOP / Info DAT / popup text, the error and warning strings | `source/FFT.cpp` (with `source/FFT.h` for the members) |
| which FFT library is loaded, or how it is located at run time | `source/FftBackend.h` |
| the regression tests | `tests/dsp_tests.cpp` - the check count is a signal: it must not go down |
| the benchmarks | `bench/bench.cpp` |

Two rules that are easy to break and hard to debug, both stated at length where they apply:

1. **A parameter that feeds a cached table must be added to that table's key**
   (`WindowKey` / `WarpKey` / `WeightKey` in `source/AnalysisPipeline.h`). Nothing enforces this at
   compile time, and forgetting it produces a table that is silently stale until the user changes
   that one parameter.
2. **Nothing that allocates, locks or prints may be added to the cook path.** The plugin's whole
   real-time argument is that a steady-state cook does none of the three; see *Performance* below and
   *The cost of the info chain* for what happens when that is violated.

And one convention to follow when writing a comment:

> **Refer to code by symbol, not by line number.** Comments in this project cite each other a lot
> ("who calls this", "where the key is checked"). A symbol name survives an edit; a line number does
> not, and this pass is the demonstration: adding comments shifts every line below them, and a note
> that said `source/FFT.cpp:239` was correct when written and wrong an hour later. Prefer
> `FFT::pollParameters()` and add a file name only when the symbol is ambiguous across files.

---

## Features

- **FFTW3 R2C engine**, vendored as **3.3.11 built with AVX2 + FMA** (see `3rdParty/fftw3/VERSION`), with a
  selectable planner policy (`Auto` / `Fast` / `Measured` / `Patient`) and **wisdom caching**
  (`%LOCALAPPDATA%\TD_Custom_FFT\fftwf_wisdom.txt`): measured plans are ~40 % faster than estimated ones and
  only cost time the first time a size is used on the machine.
- **Two FFT libraries, one node, switchable at run time**: the vendored FFTW3 build or **Intel oneMKL's FFTW3
  interface** (Performance page, `FFT Backend`). Neither is linked — both export the same `fftwf_*` symbols, so
  the plugin resolves whichever is selected with `LoadLibraryEx` + `GetProcAddress` and calls through that table.
  oneMKL is not redistributed here; see [Using Intel oneMKL instead](PluginProjects/FFT/3rdParty/fftw3/README.md#using-intel-onemkl-instead-the-fft-backend-toggle).
- **AVX2 / FMA** everywhere it pays (v2.12 pass, every change A/B-measured — see `CHANGELOG.md`): windowing,
  magnitude (rsqrt + Newton step, ~23-bit accuracy), warp interpolation (one unaligned load + `vpermps` per tap
  where the 8 output bins' taps fit one 8-float window, `vgatherdps` elsewhere), weighting fused with the dB
  reference peak, a table-free `FastLog2Seg` `20·log10` for dB (no gather, 0.00016 dB max error), in-place
  ballistics, a branch-free 4-chain peak search, and vectorised spectral features.
- **Psychoacoustic scales**: Logarithmic, Mel, ERB, Bark, Chroma, Linear, Mel+Log blend, with a `Warp Blend`
  slider and an identity (memcpy) bypass when the grid is exactly linear. Where one output bin covers several
  FFT bins, `Warp Aggregation` decides what it reports (Peak by default, so no narrow peak is ever dropped).
- **Output length in the user's hands**: `Output Bins Mode = Auto` (default) outputs N/2+1 samples of the FFT
  actually run (the zero-padded length), `Fixed` outputs exactly `Output Bins` samples — including far more
  than N/2+1 (interpolated). `Raw RFFT Bins` outputs the untouched rfft magnitude, DC..Nyquist, as an identity
  `memcpy`. See [How the resampling works](#how-the-resampling-works).
- **Zero-padding** on by default (`Zero-Pad Len`, 16384) and switchable off, in which case the FFT runs on the
  window itself.
- **Quality presets** (Visual 60 / Visual 120 / Analysis) that set the pad, interpolation, Kaiser beta mode and
  aggregation together; they never change the output count.
- **Equal-loudness weighting**: A (IEC 61672), C, ITU-R 468.
- **dB modes** with **dB Reference**: Frame Peak (legacy, 0 dB = loudest bin), 0 dBFS (absolute), Slow AGC.
- **Magnitude normalization**: Coherent Gain (legacy, `mean(window) = 1`) or Full Scale (sine amplitude 1 → 1.0).
- **Window length** in samples (legacy) or in **milliseconds** (sample-rate independent).
- **Ballistics** as per-frame coefficients (legacy) or in **milliseconds** (frame-rate independent, uses `OP_TimeInfo`).
- **Async analysis with a hard real-time cook thread**: the FFT runs on a worker; the cook thread only ingests and copies.
  Job and result handoff are wait-free triple buffers (`AsyncAnalysis`, v2.10); with `Worker Wake = Poll` (default) the
  worker polls every 2 ms on a high-resolution timer (no kernel wake-up per cook) — no mutex, no syscall, no allocation
  on the cook thread after warm-up. `Worker Priority` can register the worker with MMCSS "Pro Audio".
- **Diagnostics**: Info CHOP (36 channels: `cook_time_us`, `dsp_time_us`, `peak_freq_hz`, `hold_frames`, `jobs_dropped`,
  `output_bins`, `kaiser_beta`, `pickup_p50_us`/`pickup_p99_us`, `analysis_latency_ms`, `aggregated_bins`, the eight
  `feature_*` channels, …), Info DAT (plan log, `window_resolution`, `resolution`, `info_callback_calls`, …),
  middle-click popup, warning/error strings, AVX2 CPU guard (no illegal-instruction crash).
- **Spectral features** (optional, off by default): centroid, rolloff, flatness, flux, RMS and bass/mid/high band
  levels as Info CHOP channels, computed on the worker.
- Textport logging through `PySys_WriteStdout` (no Python script injection).

## Project layout

```text
PluginProjects/FFT/
├── CMakeLists.txt        <-- short: include(PluginBuilder_V2/cmake/TDPlugin.cmake) + td_add_plugin(...)
├── plugin.json           <-- manifest read by PluginBuilder (family, optype, deps)
├── _c.bat / _b.bat       <-- standalone configure / build helpers (vcvars64 + cmake); see Building > Standalone
├── source/
│   ├── DSPModules.h      <-- TouchDesigner-independent DSP (FIFO, EQ, window, warp, weighting, dB, ballistics, FFTW engine)
│   ├── AnalysisPipeline.h/.cpp <-- the stage ORDER and the table caches; the only caller of DSPModules.h
│   ├── FftBackend.h      <-- FFT library registry + runtime loader (FFTW3 / oneMKL, no import library)
│   ├── RateModel.h       <-- window length / FFT size / bin count / axis arithmetic + applyPreset, shared by node, bench and tests
│   ├── AsyncAnalysis.h/.cpp <-- cook <-> worker handoff (wait-free triple buffers, wake policy, priority), TD-free
│   ├── FFT.h / FFT.cpp   <-- the CHOP operator (API 10 entry points, cook, telemetry, Info CHOP/DAT, popup)
│   └── Parameters.h/.cpp <-- typed parameter definitions (enum classes, single eval() per cook, grey-out states)
├── tests/dsp_tests.cpp   <-- headless golden-vector tests (748 checks at v2.12.0, incl. lock-free handoff stress)
├── bench/bench.cpp       <-- per-stage benchmark + cook-thread simulation (--cook N, --info N, --backend fftw3|mkl, --gate)
├── bench/perf_baseline.json <-- baseline for the perf-regression gate (`ctest -L perf`)
├── bench/fftw_threads_probe.cpp <-- measures whether FFTW's built-in threading helps (it does not)
├── bench/fftw_version_probe.cpp <-- reports the vendored library's version and available flags
└── 3rdParty/fftw3/       <-- vendored FFTW 3.3.11 AVX2 (VERSION record, header, .def/.lib, runtime DLL)
```

A working copy may also have an `FFT_REFERENCE/` folder at the repository root. It is a local-only reference
folder (git-ignored, not part of the repository), so nothing in the build, the tests or this documentation
depends on it.

Three of the source files are header-only by design: `DSPModules.h`, `FftBackend.h` and `RateModel.h`
contain their implementations in the header and are included by more than one translation unit (the
node, the tests and the bench). Their free functions are declared `inline` for that reason. Before
compiling one of them standalone, or moving a function out of one into a `.cpp`, check who else
includes it - the tests and the bench link against these headers directly and would otherwise be left
with an undefined symbol at link time, not at compile time.

## Building

### From TouchDesigner (PluginBuilder_V2)
Drop `PluginBuilder.tox` into `Plugin_FFT.toe`, type `FFT` as the plugin name — PluginBuilder finds the
existing project (via this folder's `plugin.json`), configures, compiles on every source save and
hot-reloads the DLL (rename-in-place, no unload gap). Requires `PLUGIN_BUILDER_DIR` to resolve to
`../PluginBuilder_V2` (the CMakeLists default); rebuild after builder upgrades with
`python ../PluginBuilder_V2/dev/ci.py --project PluginProjects/FFT` if you want a headless check.
The generated/loader path, `plugin.json` manifest, `__Plugins__/FFT/` deploy folder and the CMake
functions (`td_add_plugin`, `td_plugin_use_fftw3 ... DYNAMIC`, `td_plugin_optimize`) all come from the
sibling [PluginBuilder_V2](../PluginBuilder_V2) repo; `CMakeLists.txt` resolves it as
`../../../PluginBuilder_V2` (override with `-DPLUGIN_BUILDER_DIR=`). Headless cross-check from that
repo: `python dev/ci.py --project ../Plugin_FFT/PluginProjects/FFT`.

### Standalone
```cmd
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd PluginProjects\FFT
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build --output-on-failure          # DSP unit tests
build\bin\Release\fft_bench.exe --channels 8        # per-stage timings
build\bin\Release\fft_bench.exe --channels 1 --db 0 --weight 0 --ball 0 --cook 300   # cook-thread cost (async / inline)
build\bin\Release\fft_bench.exe --info 2000         # info-callback / middle-click access cost
```

`_c.bat` and `_b.bat` in `PluginProjects/FFT/` are that sequence with the absolute paths already filled
in: `_c.bat` configures, `_b.bat` builds. Extra arguments pass through to CMake / the build, e.g.
`_b.bat --target fft_tests` builds one target, or `_c.bat -DFFT_BUILD_BENCH=OFF` configures without the
benchmark. Both call `vcvars64.bat` themselves, so they do not need a Developer Command Prompt - but they
must be invoked with an absolute path from PowerShell. Delete `build/` when changing the vendored FFTW
version: `td_plugin_use_fftw3` resolves the library from the version tag, and a stale `CMakeCache.txt`
would keep pointing at the previous one.

`fft_tests` prints a check count as well as a pass/fail. **748 checks, 0 failures** was the baseline at
v2.12.0 (607 before the v2.10 pass), and the count is deliberately treated as a signal: a refactor that removes a check is as
suspicious as one that fails one, so compare the number, not just the exit code. The oneMKL and
from-wisdom test branches are genuinely machine-dependent - a different total on another machine is not
a regression, but a different total on this one is.

`PLUGIN_BUILDER_DIR` defaults to the sibling `../../../PluginBuilder_V2`; pass `-DPLUGIN_BUILDER_DIR=` otherwise.
A standalone build deploys `FFT.dll` + `libfftw3f-3.3.11-avx2.dll` into `__Plugins__/FFT/` (rename-in-place).
The oneMKL DLLs are never deployed by the build: they are entirely optional and the user's to install (the plugin
only *looks* for them, in its own directory). The deployment set is 14 `.3.dll` files, ~456 MiB: `mkl_rt`,
`mkl_core`, `mkl_sequential`, the kernel set `mkl_def` / `mkl_mc3` / `mkl_avx2` / `mkl_avx512` / `mkl_avx10` and the
VML set `mkl_vml_def` / `mkl_vml_mc3` / `mkl_vml_avx2` / `mkl_vml_avx512` / `mkl_vml_avx10` / `mkl_vml_cmpt`, plus the
`oneMKL-licenses/` folder. `mkl_intel_thread`, `mkl_tbb_thread` and `libimalloc.dll` are **not** needed: the plugin
forces oneMKL's sequential threading layer at load. Only five of them load on a given CPU (on the i9-13900H:
`mkl_rt`, `mkl_core`, `mkl_sequential`, `mkl_avx2`, `mkl_vml_avx2`, ~177 MiB); the other kernel variants are there
for AVX-512 and older CPUs. On this machine they are installed in `__Plugins__/FFT/`, which `.gitignore` keeps out of
the repository — so a fresh clone builds and runs on FFTW3 alone, and the `FFT Backend` toggle is the only thing
that needs them. Details in [`3rdParty/fftw3/README.md`](PluginProjects/FFT/3rdParty/fftw3/README.md#using-intel-onemkl-instead-the-fft-backend-toggle).

Requirements: Windows 10/11 x64, Visual Studio 2022/2026 C++ tools, CMake ≥ 3.21, Ninja, a CPU with AVX2 + FMA.

## Parameters

In dialog order (the order `setup()` in `source/Parameters.cpp` registers them). Greying out is live: since
v2.10 the node tells TouchDesigner which parameters the current settings make inert (`setEnableStates()`).

| Page | Parameter | Type | Default | Notes |
|---|---|---|---|---|
| Spectrum | Quality Preset | Menu | **Custom** | Custom = every parameter below is in charge · Visual 60 fps (pad 8192, cubic, Auto β, Peak) · Visual 120 fps (pad 4096, cubic, Auto β, Peak) · Analysis (pad 32768, linear, Auto β, RMS). A preset overrides Zero-Pad Len, Warp Interpolation, Kaiser Beta Mode and Warp Aggregation (greyed out while it is active); it **never** changes the output count — Output Bins and Output Bins Mode stay yours |
| Spectrum | Channels | Menu | **Mono Mix** | Mono Mix (average all inputs → 1 analysis channel) / First Channel / All Channels (one FFT per channel) |
| Spectrum | Raw RFFT Bins (no interpolation) | Toggle | Off | On = the rfft magnitude untouched: N/2+1 samples, DC..Nyquist, bin k = k·sr/N Hz (identity memcpy). Scale, Display Max, Warp Blend, Log Floor, Output Bins, Output Bins Mode, Warp Interpolation and Warp Aggregation are bypassed and greyed out; weighting, dB and ballistics still apply (they act per bin) |
| Spectrum | Scale | Menu | Log | Log / Mel / ERB / Bark / Chroma / Linear / Melog |
| Spectrum | Display Max Hz | Float | 24000 | clamped to Nyquist; slider max 192000 so the full band is reachable at any input rate |
| Spectrum | Output Bins Mode | Menu | **Auto** | Auto = N/2+1 of the transform actually run (Zero-Pad Len 16384 → 8193 samples; the window with Zero-Padding off), laid out on Scale / Display Max; Output Bins has no effect · Fixed = Output Bins is the exact output sample count (any count, more than N/2+1 included: interpolated) |
| Spectrum | Output Bins | Int | 16384 | output samples per channel in Fixed (hard-clamped 8…262144; slider 256…65536). Greyed out in Auto and with Raw RFFT Bins |
| Spectrum | Warp Blend | Float | 0.963 | 0 = linear grid, 1 = fully perceptual |
| Spectrum | Warp Interpolation | Menu | Linear | Linear (2 taps) / Cubic Catmull-Rom (4 taps; a 16K FFT + cubic looks like 32K + linear at half the cost) |
| Spectrum | Warp Aggregation | Menu | **Peak** | What an output bin reports when it covers several FFT bins (the coarse, usually high-frequency, part of a perceptual axis): Off = interpolate between two FFT bins (legacy; a narrow peak between the taps is skipped) · Peak = the largest FFT bin in the range (no peak is ever dropped) · RMS = the power mean of the range (energy-faithful). Only those bins are affected; Info CHOP `aggregated_bins` counts them |
| Spectrum | Log Floor Hz | Float | 20 | lowest frequency of the Log / Melog grid |
| Spectrum | Window Length Mode | Menu | Samples | Samples (legacy) or Milliseconds |
| Spectrum | Window Sampling | Int | 3175 | analysis window in samples (= 72 ms @ 44.1 kHz; slider 1…32768, hard limit 65536) |
| Spectrum | Window Length ms | Float | 72 | used when mode = Milliseconds (slider 1…1000, accepted 0.1…5000) |
| Spectrum | Zero-Padding | Toggle | **On** | Off = the FFT runs on the window itself (N = window length rounded up to even, so the last bin is exactly Nyquist); Zero-Pad Len is greyed out. A power-of-two window is the fast case (the default 3175 → 3176 = 8·397 plans a slower FFT) |
| Spectrum | Zero-Pad Len | Menu | 16384 | FFT size with Zero-Padding on (1K…64K; auto-grown to ≥ next pow2 of the window). Overridden by a Quality Preset |
| Spectrum | FFT Planner | Menu | Auto | Auto: instant plan now, measured plan upgraded in the background (wisdom-cached) · Fast (Estimate only) · Measured (blocking, once per size) · **Patient**: like Auto but the background upgrade is `FFTW_PATIENT`, which executes 10–15 % faster than a MEASURE plan at 16K/32K (9.70 vs 10.97 µs at 16K, i9-13900H). The PATIENT search has no time limit (v2.12.1): about 2.7 s of above-normal-priority planning at N = 32768 on the i9-13900H, longer on slower machines, once per size per machine and never on a TouchDesigner thread; the resulting wisdom is also used by Auto. Only the FFTW3 backend honours the policy |
| Spectrum | Input Ingest | Menu | Auto | Auto = only the samples that are new since the last cook, from the input's start index and cook count (an overlapping or re-delivered buffer is not appended twice) · Append All = the newest block, every cook (the pre-2.10 behaviour) |
| EQ | EQ Enable | Toggle | **Off** | Off = no EQ code and no EQ parameter reads at all |
| EQ | High Shelf / Low Shelf | Toggle | On / On | per-shelf bypass (only read when EQ Enable is on) |
| EQ | High Boost dB, High Cutoff Hz, Low Boost dB, Low Cutoff Hz, EQ Q Factor, EQ Blend Amount | Float | 6 / 1000 / 0 / 200 / 0.707 / 1 | RBJ shelving EQ applied at ingest to new samples (stateful, 3.6 µs/channel on the i7-class desktop) |
| Window & Weighting | Window Type | Menu | Kaiser | Kaiser / Hann / Hamming / Blackman / Blackman-Harris / Rectangular |
| Window & Weighting | Kaiser Beta Mode | Menu | **Manual** | Manual = Kaiser Beta · Auto = the smallest β whose sidelobes sit below dB Range Floor (the sharpest main lobe the display can use; the presets use it). Kaiser window only; greyed out under a preset |
| Window & Weighting | Kaiser Beta | Float | 15 | Manual mode only (slider 1…55, accepted 0…100). Info CHOP `kaiser_beta` shows the β actually in use |
| Window & Weighting | Loudness Weighting | Menu | Off | Off / A / C / ITU-R 468 |
| Window & Weighting | Magnitude Normalization | Menu | Coherent Gain | Full Scale makes a sine of amplitude 1 read 1.0 |
| Loudness & Ballistics | Loudness Mode | Menu | Off | Off (linear) / dB / dB normalized 0…1 |
| Loudness & Ballistics | dB Reference | Menu | Frame Peak | Frame Peak / 0 dBFS / Slow AGC |
| Loudness & Ballistics | dB Range Floor | Float | 80 | read when a dB mode is on, or when Kaiser Beta Mode = Auto designs the window from it |
| Loudness & Ballistics | Ballistics Enable | Toggle | **Off** | Off = no ballistics code and no attack/release parameter reads |
| Loudness & Ballistics | Ballistics Mode | Menu | Coefficient | Coefficient (per frame) or Milliseconds |
| Loudness & Ballistics | Attack / Release Speed | Float | 0 / 0 | per-frame coefficients 0…0.99 |
| Loudness & Ballistics | Attack / Release ms | Float | 50 / 200 | used when mode = Milliseconds |
| Loudness & Ballistics | Reset | Pulse | | clears ballistics, AGC and EQ state |
| Performance | Async Analysis (worker thread) | Toggle | **On** | FFT & post-processing on a worker thread; the cook only ingests and copies (≈ 11 µs at 16384 bins, 7 µs at 4096, measured on the i7-class desktop). Off = inline, and **one thread for the whole node**: no worker, and no background plan measurement either |
| Performance | FFT Backend (off: FFTW3 / on: Intel oneMKL) | Toggle | **Off** | Off = the vendored FFTW3 3.3.11 AVX2 build. On = Intel oneMKL's FFTW3 interface (`mkl_rt.3.dll`), which runs the FFT 18–27 % faster on the i9-13900H (see *Which FFT library is faster*) but has to be installed by the user. Missing DLL = logs and falls back to FFTW3, never a planless node. A toggle rather than a menu because there are two libraries; the registry in `FftBackend.h` and the `static_assert`s in `Parameters.cpp` are what a third would extend |
| Performance | Worker Wake | Menu | Poll | Poll = the worker checks the job slot every 2 ms (no kernel call on the cook thread; pickup 0–2 ms) · Signal = the cook wakes it every cook (one `SetEvent`, ~5–25 µs on the cook thread; pickup ~0.02 ms). The worker goes dormant after 500 ms idle either way. Greyed out with Async off |
| Performance | Worker Priority | Menu | Highest | Highest = `THREAD_PRIORITY_HIGHEST` · MMCSS Pro Audio = registered with the Multimedia Class Scheduler, which schedules it ahead of normal threads and exempts it from power throttling (for 120+ fps projects on a busy machine). Applied at thread start; greyed out with Async off |
| Performance | Spectral Features (Info CHOP) | Toggle | Off | On = eight `feature_*` Info CHOP channels (centroid, rolloff, flatness, flux, RMS dB, bass / mid / high dB), computed on the worker from the linear magnitude. ~3.5 µs per analysis at 8193 bins since v2.12 (was ~16.5 µs) |

Defaults (Coherent Gain, Frame Peak, Samples, Coefficient, Kaiser β 15 Manual; EQ and Ballistics **off**) reproduce the
processing of the early builds, in which the EQ was inactive, with two deliberate differences since v2.10/v2.11:
the output is N/2+1 of the padded FFT (8193 samples at the default pad, `Output Bins Mode = Auto`) rather than 16384,
and output bins that cover several FFT bins report their peak (`Warp Aggregation = Peak`) instead of interpolating.
Every optional section is bypassed entirely — code *and* parameter reads — when disabled: `eval()` reads **30**
parameters per cook in the default configuration, against **43** with every optional section on (EQ with both
shelves, a dB mode, ballistics; Kaiser Beta Mode = Manual). Those two numbers are counted from `eval()` at v2.12.0
(they were 20 / 33 before the v2.10 parameters). The live count is not an estimate: `eval()` increments it and
hands it back, and it is reported as the Info CHOP channel `param_reads`, so the numbers in this paragraph can be
checked against a running node. Every parameter is read on every cook, so a change takes effect on the next frame.

## How the resampling works

The FFT produces a fixed linear grid — `fft_size/2 + 1` bins, DC to Nyquist, every bin `sr_in/fft_size` Hz apart
(43.07 Hz at 44.1 kHz and N = 1024; 1.35 Hz at N = 32768). The output length `n_out` says how many samples
*describe that same spectrum*: with `Output Bins Mode = Auto` (the default) it is `fft_size/2 + 1` of the transform
actually run — 8193 at the default Zero-Pad Len of 16384 — and with `Fixed` it is exactly `Output Bins`, any count,
more than `fft_size/2 + 1` included (`RateModel.h`, `outputBinCountFrom`). Unless `Raw RFFT Bins` is on, the node then
resamples the linear grid onto a new frequency axis of `n_out` bins:

1. **Target axis.** For each output bin `i` of `n_out`, `computeTargetHzGrid` picks the frequency it should
   represent: `target_hz[i] = (1 - blend) * i/(n_out-1) * fmax + blend * perceptual(i)`, where `perceptual()` is the
   chosen scale (Log / Mel / ERB / Bark / Chroma) mapped from `log_floor` to `fmax`, and `fmax = min(Display Max,
   Nyquist)`. With `blend = 0` (or Scale = Linear) the axis is uniform: `n_out` bins evenly covering 0…fmax.
2. **Gather tables.** `buildWarpTables` converts each target frequency into a fractional position in the *linear*
   grid: `frac = target_hz[i] / nyquist * (fft_size/2)`, then stores `i0 = floor(frac)` and `w = frac - i0` into two
   tables (8 bytes per output bin).
3. **Interpolation.** `applyWarp` walks the output and reads `src[i0] + w * (src[i0+1] - src[i0])` — a 2-tap linear
   interpolation, or a 4-tap Catmull-Rom cubic with `Warp Interpolation = Cubic`. Since v2.12, 8 output bins whose
   taps all fall inside one 8-float window (the fine, upsampled part of the axis — ~83 % of the vectors at a
   16384-bin Log axis) read each tap with one unaligned load plus one `vpermps`; the rest use an AVX2 `vgatherdps`.
   Both paths are bit-identical.
4. **Aggregation.** Where one output bin covers two or more FFT bins (the coarse end of a perceptual axis),
   `Warp Aggregation` replaces the interpolated value with the largest FFT bin in the range (Peak, the default) or
   its power mean (RMS); Off keeps the interpolation. Since v2.12 the trailing run of aggregated bins is not
   interpolated first, since aggregation overwrites it anyway.

So going from 16384 bins to 16384 bins does **not** upsample anything: the same band is described by a different
number of samples. Fewer bins means the axis is coarser (each output bin covers a wider slice of the linear grid,
and `Warp Aggregation` decides what it reports); more bins than `fft_size/2+1` means bins are interpolated
*between* real FFT bins — smooth, but with no information that was not already there. The one case with no loss is
the identity grid, where `isIdentity()` is true and the warp is a `memcpy`: the output is the linear FFT grid
itself, copied, with no interpolation step and no rounding, every bit the transform produced. There are two ways
to get it:

- **`Raw RFFT Bins` on** (v2.11). The toggle forces it: N/2+1 samples, DC..Nyquist, whatever the axis parameters
  say — and it greys those parameters out, so the two spellings cannot disagree. This is the one to use.
- **The equivalent setting**: Scale = Linear, `blend = 0`, `Display Max >= Nyquist` and `n_out = fft_size/2+1`
  (Output Bins Mode = Auto, or Fixed with that count). The warp detects it on its own. (v2.8 removed an older
  `Raw Linear Bins` toggle because it was a second spelling of this setting that could disagree with it; the v2.11
  toggle avoids that by greying the axis parameters out while it is on.)

### Output sample rate

The CHOP reports **`output_sample_rate = output bins × me.time.rate`**: one output vector of `bins`
samples is produced every `1/me.time.rate` seconds, so at the defaults (Output Bins Mode = Auto, Zero-Pad
Len 16384 → 8193 bins, 60 fps) that is **491 580 samples/s** (983 040 with `Fixed` and 16384 bins). `me.time.rate` is the timeline rate *where the node lives*
(`OP_TimeInfo::rate`), so inside a component with Component Time the component's rate is used, not
the root's; it is read every cook, so an FPS change shows up on the next frame.

Read this as the rate of the frames **concatenated**: it says how fast spectrum data leaves the
node, which is what you size a buffer, a ring, a GPU upload or a network send with. It is not a
property of the spectrum itself, so it carries **no bin-index-to-Hz information** — a 16384-bin
vector describing 0…Nyquist and a 16384-bin vector describing 0…10 kHz both report 983 040 at 60 fps.
**To convert a bin index to Hz, use `hz_per_sample` or `output_spectrum_axis`, never the sample
rate.**

#### The frequency axis

The band the bins actually describe is tracked separately, in the standard "bin 0 is DC, the last
bin is Nyquist" form: **`axis rate = 2 × (top of the axis)`**.

| Configuration | Axis (44.1 kHz input) |
|---|---|
| `Raw RFFT Bins` on, or the identity setting (`Scale = Linear`, `blend = 0`, Display Max ≥ Nyquist, `n_out = fft_size/2+1`) | **44100 Hz** — bin `i` is at `i*44100/fft_size` Hz, the transform's own grid (Raw RFFT Bins ignores Display Max) |
| Scale = Linear, `blend = 0`, Display Max ≥ Nyquist, any other output count | **44100 Hz** — the band is unchanged, only how many bins describe it |
| Display Max = 10000, any output count, any uniform scale | **20000 Hz** — the axis really stops at 10 kHz |
| Log / Mel / ERB / Bark / Chroma (non-uniform bins) | **2 × min(Display Max, Nyquist)** — exact at both ends (DC/floor … fmax); no single spacing describes a non-uniform grid in between |

`hz_per_sample = axis rate / (2 * (bins - 1))` — for the identity grid that is
`44100 / (2 * fft_size/2) = sr_in/fft_size`, the transform's own resolution. The axis rate does not
depend on how many output bins you chose: resampling changes how a band is described, not how wide
it is (`hz_per_sample` does, since the same band is split into more or fewer steps).

#### Which channel to read

| Info CHOP channel | Meaning |
|---|---|
| `output_sample_rate` | `bins × me.time.rate` — what the CHOP reports to TouchDesigner (491 580 at the defaults: 8193 bins × 60 fps) |
| `output_bins` | The output length actually declared: `Output Bins` in Fixed, `fft_size/2+1` in Auto or with Raw RFFT Bins. The Info DAT `resolution` row spells out which: `N output bins (Fixed \| Auto = N/2+1 \| Raw rfft, fft N zero-padded \| no padding)`, followed by the Kaiser β, the aggregation, the preset and the ingest mode |
| `hz_per_sample` | Hz per bin, from the axis — **the bin-index-to-Hz conversion**. Exact for the identity grid and any uniform axis; the mean for a perceptual one |
| `output_spectrum_axis` (Info DAT) | The axis itself: `hz_per_sample × bins = 0…top` |
| `output_bandwidth_sps` | Measured throughput, `bins × (frames actually producing a spectrum)/s` — the same number as `output_sample_rate` computed from the cook delta actually observed rather than from `me.time.rate` |
| `linear_grid` | 1 when the built warp is the identity, i.e. the output grid *is* the linear FFT grid and the magnitude was copied rather than resampled. Read from the warp tables, not from a parameter, so it cannot disagree with what the DSP did |
| `channel_fanout` | 1 when the last cook fanned its channel loop out over cores. 0 is the normal reading: `Mono Mix` (the default) has one channel and nothing to split, and several mono channels means several node instances |

`getInfoPopupString` (middle-click the node) prints all of them, plus the cook delta the throughput
was measured over.

## Performance (fft_bench, 1 channel, N = 32768, 16384 bins, Log)

**Which machine a row came from matters more than the row does**, so each block names one. Absolute times on a
laptop move with thermals and with the plan that is live; the direction of a comparison is the durable part.

**The current numbers are the v2.12.0 block** ([*Current numbers: v2.12.0*](#current-numbers-v2120-i9-13900h)) and
the library A/B after it. The first two tables are kept as history: they were measured on earlier builds, before
the v2.10–v2.12 real-time and SIMD passes, and the heading above describes their configuration, not the current
defaults (which output 8193 bins from a 16384-point FFT).

The table below is the historical record from the original development run ("i7-class desktop", earlier commits,
plans as built then). It is kept because the *ratios* between configurations are what the parameters are tuned
against, and because the history paragraph after it refers to it. It is **not** comparable to the i9 numbers
that follow it.

| Configuration | FFT+mag | warp | EQ | dB | total / channel |
|---|---|---|---|---|---|
| **Default TD config** (Loudness/Weighting/EQ/Ballistics off), cold plan | 41–48 µs | 4.8 µs | – | – | **44–51 µs** |
| Same after the background measured plan is in (or wisdom cached) | ~36 µs | 4.8 µs | – | – | **~42 µs** |
| + EQ Enable (6 dB high shelf, applied at ingest) | | | 3.6 µs | | +4 µs |
| Everything on (dB, A-weighting, ballistics, EQ) | 42–50 µs | 5 µs | 3.6 µs | 3.5 µs | **~60–70 µs** |
| N = 16384 + **Warp Interpolation = Cubic** (visually equivalent to 32K linear) | 15–16 µs | 9 µs | – | – | **24–27 µs** |
| N = 8192 | 6.4 µs | 4.7 µs | – | – | **~15 µs** |
| **Async on** (default): cost on the cook thread, any N, measured with `fft_bench --cook` (caches evicted between cooks) | – | – | – | – | **≈ 11 µs mean / 17 µs p99** at 16384 bins, **≈ 7 µs** at 4096 bins (ingest 2 + snapshot 2 + result copy 2–7; DSP runs on the worker) |

Re-measured on the development machine — **i9-13900H** (Raptor Lake, 6 P-cores + 8 E-cores / 20 threads, AVX2 + FMA, no AVX-512),
FFTW3 3.3.11 AVX2 with a `FFTW_MEASURE` plan from wisdom, same 300-iteration `fft_bench` invocation, on a build from
before the v2.10–v2.12 passes (also history now; see the v2.12.0 block for current figures):

| Configuration | FFT+mag | warp | dB | ballistics | total / channel |
|---|---|---|---|---|---|
| Default TD config, measured plan from wisdom | 22.45 µs | 4.66 µs | – | – | **28.96 µs** |
| Everything on (dB + A-weighting + ballistics; EQ unchanged) | 23.57 µs | 4.74 µs | 3.53 µs | 1.29 µs | **39.45 µs** |

Two honest notes on that block. First, `+ EQ Enable` re-measured at **0.01 µs**, i.e. it does not reproduce the
historical "+4 µs EQ" row on this machine — treat the EQ as free at the default shelf settings until a run says
otherwise. Second, the historical default row is 44–51 µs against this machine's 28.96 µs, but the two ran on
different builds, plans and CPUs, so that is **not** a speed-up claim; the controlled comparisons are the
library A/B below and the `--cook` Async numbers, which hold everything but the variable under test constant.

### Current numbers: v2.12.0 (i9-13900H)

The v2.12 AVX2/FMA pass (load + permute warp, table-free `FastLog2Seg` dB, branch-free peak search, vectorised
spectral features, in-place ballistics, four-accumulator reductions, weighting fused with the dB peak) was
A/B-measured against the committed v2.11 `fft_bench`: interleaved in random order, pinned to one P-core at high
priority, 3 rounds on an idle CPU, medians in µs. FFTW3 backend.

| Metric (µs, median) | v2.11 | v2.12 | Speedup |
|---|---|---|---|
| pipeline, full chain (dB + ballistics + features) | 36.35 | **22.20** | **1.64×** |
| pipeline, plugin defaults | 19.10 | **17.60** | 1.09× |
| pipeline, Visual 60 preset | 9.90 | **8.90** | 1.11× |
| cook, synchronous (Async off) | 28.15 | **25.20** | 1.12× |
| stage: warp, Log linear, 16384 bins | 4.50 | **2.52** | **1.79×** |
| stage: warp, Log cubic, 16384 bins | 8.08 | **4.82** | **1.68×** |
| stage: dB normalised, 16384 bins | 4.22 | **3.58** | 1.18× |
| stage: peak + index, 16384 bins | 1.26 | **0.94** | 1.34× |
| spectral features, 8193 bins | ~16.5 | **~3.5** | ~4.7× |

With the post-processing this lean, the FFT itself is now most of the default cost — roughly two thirds, derived
from two separate runs (11.55 µs FFTW3 FFT at N = 16384, below, against the 17.60 µs default pipeline) — so the
levers that matter are `Zero-Pad Len` and the `FFT Backend`. The full A/B, the second pass (reductions) and the
optimisations that were measured and **not** kept (a fused dB + ballistics + peak kernel, hardware `sqrt`, polynomial
`log2`, …) are in `CHANGELOG.md`, v2.12.0; the reproducible gate is `ctest -L perf` against
`bench/perf_baseline.json`.

### Which FFT library is faster: FFTW3 vs Intel oneMKL

**Current measurement (2026-09-23, v2.12 build, i9-13900H, median of 3 pinned runs, FFT stage only):**

| N | FFTW3 3.3.11 AVX2, `FFTW_MEASURE` plan | Intel oneMKL 2026.1.0 | oneMKL faster by |
|---|---|---|---|
| 8192 | 4.71 µs | 3.87 µs | 18 % |
| 16384 | 11.55 µs | 8.41 µs | 27 % |
| 32768 | 23.65 µs | 17.73 µs | 25 % |

An `FFTW_PATIENT` plan narrows the gap without closing it: 9.70 µs at 16K, so oneMKL still leads by ~13 %. FFTW3
stays the default because it is 3 MB, vendored and wisdom-cached; oneMKL loads 5 DLLs (~177 MiB on this CPU) and
spends ~39 ms initialising once per process (on the worker thread when Async is on). The bench flag takes a
**name**: `--backend fftw3` or `--backend mkl`. `--backend 1` is not an index — it is rejected and the run falls
back to FFTW3, with a single "unknown --backend" line that is easy to miss; that is exactly how a first comparison
in the v2.12 work came out backwards (both runs were FFTW3).

The earlier paired runs (v2.9.0 build, same machine) — same binary, same bench invocation, only the library
differing (`--backend fftw3` vs `--backend mkl`), N = 16384, 16384 bins:

| library | fft+mag | total per cook |
|---|---|---|
| FFTW3 3.3.11 AVX2, `FFTW_MEASURE` plan from wisdom | 11.94 µs | 21.06 µs |
| Intel oneMKL 2026.1.0 | 8.85 µs | 17.63 µs |

Four paired runs each way put oneMKL **13–34 % faster on the fft+mag stage** every time. That is the measured
reason the `FFT Backend` toggle exists; a re-run after a clean rebuild reproduced it (mkl 9.66 µs vs fftw3
14.77 µs on fft+mag, 4/4 pairs, −35 %). Those runs timed fft+mag rather than the FFT alone, with the older
paired method; the pinned 2026-09-23 medians above (18–27 %) sit inside that range and are the figures to quote.

**The A/B has to be interleaved, or it reports the opposite answer.** Each `fft_bench` process pays a cold
first-plan and cold-cache cost on whatever library it loads, so a single fftw3 run followed by a single mkl run
puts the *second* library ahead regardless of which is faster — measured here at 15.65 µs (fftw3, run second)
against 16.82 µs (mkl, run first), inverting the table above. Alternate the two backends within one loop and
re-run at least four pairs before believing any of it; the deployment notes, the OpenMP hazard it avoids and the list of DLLs
it needs are in [`3rdParty/fftw3/README.md`](PluginProjects/FFT/3rdParty/fftw3/README.md#using-intel-onemkl-instead-the-fft-backend-toggle).

In the historical i7-class table the FFT was 75–85 % of the default cost. History (same bench on every commit):
the July builds measured 47 µs (measured plan, EQ dead), `d60b7e3` turned the EQ on (+16 µs), `2daf9f1` switched to
ESTIMATE plans (+18 µs), `f1cb0d0`–`2daf9f1` had a scalar-log10 dB stage (+45 µs when dB was on).

### Threading

Two threads, both named for the debugger, **neither below normal priority**:

| Thread | Priority | Runs |
|---|---|---|
| analysis worker (`FFT Custom CHOP analysis`) | `THREAD_PRIORITY_HIGHEST` (default), or MMCSS "Pro Audio" with `Worker Priority = MMCSS` | window → FFT → magnitude → warp → weighting → dB → ballistics → peak (→ spectral features) |
| FFTW background planner (`FFT background planner`) | `THREAD_PRIORITY_ABOVE_NORMAL` | one `FFTW_MEASURE`/`FFTW_PATIENT` plan per FFT size, then exits |

The worker sits one notch above the planner so a cook always wins the core back from it; the planner is above
normal so a `FFTW_PATIENT` measurement is not starved under load. Since v2.12.1 PATIENT has **no time limit**
(v2.10–v2.12 capped it at 1.5 s with `fftwf_set_timelimit`): it runs off the cook thread, so a slower machine may
take as long as it needs to find the best plan (~2.7 s at N = 32768 on the i9-13900H), once per size per machine,
then wisdom caches it. FFTW's planner is process-wide, so a re-plan requested meanwhile (a size change, another
node) waits for the planner lock until the measurement ends: with Async on that waiter is the analysis worker and
TouchDesigner keeps cooking; with Async off it is the cook thread. Deleting the node or quitting TouchDesigner
mid-measurement waits for it too. A measurement that is no longer wanted is abandoned to a graveyard rather than
blocking a cook.

**FFTW's own threading does not help this node.** `fftwf_init_threads` / `fftwf_plan_with_nthreads` were exported by
the unversioned `libfftw3f-3.dll` the project used before (the current 3.3.11 build is configured without threads and
exports neither, which costs nothing — the plugin never called them), but FFTW parallelizes the `howmany` loop and
multi-dimensional transforms, *not* the inside of a single 1-D transform — and `Mono Mix` (the default) issues exactly
one transform per cook. Measured with `fft_threads_probe.exe` on the i7-class desktop, against that older DLL:
nthreads 1 / 2 / 4 / 6 → 68.8 / 85.3 / 81.7 / 102.5 µs, i.e. **slower** under both
`FFTW_ESTIMATE` and `FFTW_MEASURE` (−13 to −51 %). A `howmany = 4` plan does drop to 29.9 µs per transform, but that
is FFTW splitting the *batch*; the plugin gets the same effect without the plane of global FFTW state by running its
channel loop under `std::execution::par` (see below). Run the probe on your own machine before trusting the numbers.

**Where the threading actually is: not in this node.** This node is one mono channel per instance. Several
channels means several node instances, each with its own `FFT Custom CHOP analysis` worker — that is what spreads
across cores, and it needs no parameter. None of the five `Performance` controls sets a thread count: `Async`
(worker or inline), `FFT Backend` (which library), `Worker Wake` and `Worker Priority` (how the one worker is woken
and scheduled), and `Spectral Features`.

**`Async` off really does mean one thread for the whole node.** No worker is started, and the background plan
measurement — the only other thing that would run off the cook thread — is not started either: `prepare()` leaves
the node on `FFTW_ESTIMATE`, says so in the plan line ("measured upgrade deferred: Async is off") instead of
promising an upgrade, and remembers that it is owed one, so turning `Async` back on starts the measurement then
rather than leaving the node on ESTIMATE forever. `fft_tests` asserts both halves (no background thread over 20
cooks with Async off, and the upgrade arriving after it is turned back on).

**FFTW's planner is serialised, on purpose.** The manual is explicit that `fftwf_execute` (and the new-array
variants) are the *only* thread-safe routines and that everything else — the planner — "should only be called from
one thread at a time", because planner calls share wisdom and trigonometric tables. So the engine holds one
process-wide mutex around every planner call, on both threads: the cook thread's `prepare()` and the background
measurement take the same lock, as do every `destroy_plan` and the wisdom import/export. That is FFTW's own
recommended pattern (a semaphore around planner calls) and it is what makes the Async worker and the background
planner legal at all. `fftwf_make_planner_thread_safe` is deliberately not used: the manual calls it "the worst of
all worlds" and our mutex already does its job with our own priority ordering.

The internal channel loop does run under `std::execution::par` when a job has more than one channel, with no knob
either way: each channel owns its `DspState` (padded frame, magnitude, scratch, ballistics history) and the only
shared state is read-only. It applies to `Channels = All Channels` only — `Mono Mix` produces one transform per
cook, so the branch is a single integer compare and the serial path is taken. `fft_bench` measures it at 2 / 4 / 8
channels: **38 % / 65 % / 76 % faster** than serial, outputs identical bin for bin.

## The middle-click info popup: why it goes blank, and how to fix it

This was the most expensive bug in the project's history, it was misdiagnosed three times, and it is written down
here so the next person spends ten minutes on it instead of a day. **Read the mechanism first: almost every wrong
guess comes from assuming the popup asks the plugin for its text.**

### The mechanism: the popup is a *snapshot of the last cook*, not a query

TouchDesigner's middle-click does **not** call the plugin. It renders whatever the *last cook* left behind. Every
information callback runs inside a cook, in this order, documented at the top of `CHOP_CPlusPlusBase.h`:

```
getGeneralInfo -> getOutputInfo -> getChannelName xN -> execute()
  -> getNumInfoCHOPChans -> getInfoCHOPChan xN
  -> getInfoDATSize -> getInfoDATEntries xN
  -> getInfoPopupString -> getWarningString -> getErrorString
```

Two consequences follow, and they are the whole of this section:

1. **A node that is not cooking has no information surface at all.** No popup, no Info CHOP, no Info DAT, no
   warning, and - the one that actually hurts - **no `getErrorString`**, so a hard failure (a plan that will not
   build, an input with no usable sample rate) reports itself as silence instead of an error badge.
2. **There is no plugin-side change that can force a render.** If the chain is not entered, nothing written in
   `getInfoPopupString` is ever read. Any fix attempted there is a fix aimed at the wrong component.

### The two failure modes, and the one thing that tells them apart

A blank popup is one of exactly two things, and they need opposite fixes:

| | Chain **not** entered | Chain entered, text **not** rendered |
|---|---|---|
| `info_callback_calls` Info DAT row | counters **frozen** (they stop advancing while the node cooks) | counters **climb** — popup count tracks `cookCount` 1:1 |
| The popup's own character count (same row) | stale, alongside a frozen call count | steady and ordinary; **a constant now** (v2.9.1 clipped and bounded the tail, so the same node hands over the same number every cook; it was ~800-1300 before, and ~1660 in v2.8) |
| Meaning | the node stopped cooking → fix the cook, not the popup | the string is fine and TouchDesigner did not draw it → **not a plugin problem** |
| Fix | see *"Making sure the node is cooking"* below | report to Derivative with that evidence |

Both columns are read from the **same Info DAT row**, which is the point: the popup is the symptom, and the row is
the measurement. Nothing is printed to the textport for this any more — a periodic announcement line used to be
emitted from `getInfoPopupString`, and it was removed in v2.9.0 (see *"The cost of the info chain"* below for why a
print on that path is the wrong instrument).

**The popup string is never textually empty** - its first line is always `Node: <path>`, built before anything that
can throw. So a blank popup is never "the string came out empty"; it is always "the callback did not run, or ran
and was not drawn". That invariant is deliberate and should be preserved.

### The cost of the info chain: why the popup could take a frame or two to appear

This is the one part of the popup problem that was **measured and then fixed**, as opposed to correlated and left
alone. It answers the report that the info "sometimes takes time to show up, or takes multiple attempts at the
middle-click to trigger it" — with the DLL loaded and the FFT visibly running.

The callbacks are called **inside a cook**, so every microsecond they spend is added to the node's cook time on the
thread that just ran the analysis. TouchDesigner drives them one item at a time, and the counts are not small:

| Callback | Calls per cook | What each call asked for, before v2.9.0 |
|---|---|---|
| `getInfoCHOPChan` | **21** | take `myStatusMutex`, copy a `Status` (two `std::string` members → two heap allocations) |
| `getInfoDATSize` | 1 | take the log mutex, **copy all 256 log strings** to declare the row count |
| `getInfoDATEntries` | **276** | take `myStatusMutex`, copy a `Status` again — *per row* |
| `getInfoPopupString` | 1 | take `myStatusMutex` + copy a `Status`, then **copy all 256 log strings again** for a 3-line tail |

That is **~300 mutex acquisitions, ~850 heap allocations and ~512 string copies per cook**, on the real-time thread —
and on the *same* mutexes the audio worker takes when it logs a plan event, which is why it was worse around a
rebuild than in the steady state. It also flatly contradicted the "no allocation on the cook thread after warm-up"
invariant the rest of the plugin is built around.

Fixed by making the access pattern pay once per cook instead of once per call:

- **`statusSnapshot()` is memoized behind a version stamp.** The published copy only moves when the worker rebuilds
  a plan or the tables, so the reader compares an atomic counter and reuses the previous copy. (The write side was
  already gated this way; only the read side was re-copying.)
- **`PlanLog::version()` + `PlanLog::snapshotTail(n, out)`.** The DAT row view is re-taken only when the log actually
  changed, and the popup copies the three entries it renders into a reused scratch vector instead of the whole
  history.

Measured with `fft_bench --info 2000`, which drives the same access pattern against the real `PlanLog`:

```
info-callback access cost (298 status reads + the 276-row log view, per cook, 2000 cooks):
  re-lock + re-copy every call       54.6 us/cook
  memoized + version-stamped          0.6 us/cook   -99%
  saved                              54.0 us/cook   (0.32% of a 16.7 ms frame at 60 fps)
```

For scale, the analysis itself is ~88 µs/cook for one channel at these settings — so the *bookkeeping for reading
the node's own status* was costing about as much as the DSP it was reporting on. `fft_bench --info N` is kept so the
number can be re-taken rather than re-argued.

Note what this does and does not claim. It removes a large, real, measured cost from the path a middle-click query
walks, and it removes the ~850 allocations and the mutex traffic that could block the cook thread behind the audio
worker. It does **not** prove TouchDesigner's renderer was timing out; nothing on this machine can measure that.

### Rule: never put a diagnostic in the popup string

This is not a style preference; it is what broke the popup, twice in each direction, and it is the one change that
was measured. The string that renders is a fixed identity block plus a telemetry body - a set of numbers whose
length is dominated by the two paths - and then up to **three** plan-log lines, each clipped to **72 characters**.
**The whole thing is hard-bounded at 1200 characters** (`kMaxPopupChars` in `FFT.cpp`), with every append past the
identity block going through one `add` lambda that refuses a line which would not fit. That last part is a
correction, not a detail: the bound used to guard **only the tail loop**, so the ~780-character body plus one
240-character plan line could be handed over before anything was checked - the failure the bound existed to prevent,
inside the bound's own implementation. Adding two diagnostic lines - `Info callbacks entered: ...` and
`Cook stall: ...` -
took the string to roughly 1760 and the popup rendered **empty**; removing them brought it back. No size limit is
documented anywhere in the SDK (`OP_String::setString` is a bare `virtual void setString(const char* val)`, no cap
stated), so what TouchDesigner does above some undisclosed length is **not** established - but the correlation was
reproduced in both directions, and it is enough to state the rule:

> **No diagnostic may ever be added to the popup string.** Diagnostic values go in the `info_callback_calls` Info DAT
> row (row 19), which TouchDesigner reads as *a value*, not as the popup. A diagnostic that is rendered by the thing
> it is measuring can change what it measures - and here it appears to have done exactly that.

The same rule is why the popup carries a **character count** into that Info DAT row instead of printing it.

### Rule: the popup's length is a constant

A corollary of the above, and the other half of making the length signal worth reading. In v2.9.0 the popup's length
swung by hundreds of characters between cooks: the body is fixed, but each rendered plan-log line runs to ~240
characters because the engine's backend description embeds the absolute path of the FFT library, so a blank popup
beside a length of 900 or 1300 meant the same thing - nothing. v2.9.1 clips each rendered line
(`kMaxTailLineChars`, via `FFTDSP::clipLine`) and bounds the total, so the same node hands over the same number on
every cook. That is what makes the row readable: a different number now means one of the *inputs* changed (the node
path, the install path, or a plan line), never that the string outgrew something between one cook and the next.
Clipping is for the popup only - the Info DAT's `plan_log_*` rows and the textport carry the line whole, because
neither is a fixed-size surface and a clipped log line read as the log would be worse than a long popup.

### Making sure the node is cooking (branch 1)

In order of how often each is actually the cause:

1. **Is the DLL loaded in the node the one you just built?** TouchDesigner does **not** reload a DLL when the file
   on disk changes. It is easy to spend a day alternating between two different builds and calling the result
   "intermittent". Pulse **`Reloadplugin`** on the PluginBuilder COMP (the `Reloadplugin` handler in `PluginBuilderExt.py`, which calls
   `_do_copy_plugin(force=True)`), or turn the
   loader's `unloadplugin` off/on. To confirm which DLL answered, middle-click and read the `Binary:` line - it
   prints `OP_NodeInfo::pluginPath`.
2. **There are two installs, and only one of them auto-updates.** `<project>/__Plugins__/FFT/FFT.dll` is copied by
   PluginBuilder on every build. `~/Documents/Derivative/Plugins/FFT/FFT.dll` (the registered Custom Operator) is
   updated **only** by the **Install Plugin** pulse and drifts stale - it was three builds behind during the session
   that produced this note. Each instance also resolves FFTW3/oneMKL from *its own* directory, so a measurement on
   one says nothing about the other.
3. **Is the timeline advancing?** `cookEveryFrame = true` makes the node cook on every frame, not on every
   wall-clock tick. A paused timeline stops cooking, the spectrum holds its last frame and *looks* fine, and the
   popup goes blank. This is the single best explanation for "it worked a minute ago".
4. **Is an error or warning showing on the node?** TouchDesigner reports an error state **in place of** the
   operator information. Until v2.9.0 both `myErrorText` and the pipeline error string could latch forever
   (`getErrorString` was driven off a lifetime counter that is never cleared, and `myErrorText` cleared only on a
   `Reset` pulse), so one transient exception during a hot-swap left a perfectly healthy node flagged as broken
   permanently. Both now clear on the condition they describe - see `CHANGELOG.md`, v2.9.0.
5. **The viewer flag on the loader.** For a `.dll` hosted in the built-in CPlusPlus CHOP, the SDK's
   `OP_CustomOPInfo::cookOnStart` does not apply (it is Custom-Operator only), so an active **viewer** flag is the
   only kick-start available. PluginBuilder sets it (`_ensure_loader_live()`); note that `PluginBuilderExt.py` is
   loaded when the COMP is created, so a change there is inert until the extension is re-inited or the `.toe` is
   reopened.

### What is *not* established

Recorded honestly, because the confident version of this section was wrong three times:

- **Why the extra two lines blanked the popup.** The correlation is solid and reproduced in both directions; the
  mechanism is not. No SDK cap is documented, and no test on the development machine can measure TouchDesigner's
  renderer.
- **Whether the popup is stable indefinitely.** The longest confirmed run was ~4800 popup entries (80 s at 60 fps)
  at a stable 1660-1662 characters - and that same build later reported blank with no code change in between, which
  is why the cook-driven mechanism above is the one to reach for first.
- **That any given blank popup is this node's fault.** Establish which branch it is from the table above before
  changing a line of code.
- **That the info-chain cost was the *whole* cause of the slow popup.** What is measured is that the access pattern
  cost 54.6-60.7 µs/cook and now costs 0.4-0.6 µs/cook (two runs of `fft_bench --info`, both in `CHANGELOG.md`, v2.9.0). Whether TouchDesigner's own query was slow enough to be affected by
  that - or whether it re-queries, caches, or times out at all - is not observable from inside a plugin.
- **That the v2.9.1 length change fixed anything.** It removes a mechanism, it does not prove a cause. The popup
  came back on a build compiled *before* those changes, which is the same "no code change in between" behaviour
  recorded above - so what the change buys is a clean signal for the next report, not a demonstrated repair.

### What was tried, and what each attempt actually taught

| Change | Result |
|---|---|
| `cookEveryFrame = true` (`b388b5a`) | **A real bug, still needed** - `cookEveryFrameIfAsked` is the *weaker* flag ("only if someone asks"), so an idle node genuinely had no information surface. But it was **not** established as *the* popup cause: the popup emptied again afterwards with the flag unchanged. |
| One `setString` at the end, not two (`6309258`) | Kept. Two calls (short block, then full text) is not the shape observed rendering, and one call is strictly simpler. The safety the two-call order wanted is kept by appending in the `catch` blocks instead of replacing. |
| Removing the latched error states (`6309258`) | **Kept, independent real defect.** A node can no longer claim a fault it no longer has. |
| Two diagnostic lines **in the popup string** | **Removed, and the rule above exists because of it.** This is the change that separated working from blank. |
| Log line once per load, instead of every 300 cooks (`7b419b5`) | **Now removed entirely (v2.9.0).** It went one-shot, then went away: the call count and the character count it carried both live in the `info_callback_calls` Info DAT row, so the line was pure textport noise on the real-time path. |
| Memoizing `statusSnapshot()` + `snapshotTail()` (v2.9.0) | **Kept, and measured.** 60.7 → 0.4 µs/cook in one `fft_bench --info` run, 54.6 → 0.6 in the run quoted above. This is the only change in this table that was made against a number rather than against a correlation. |
| Bounding the popup **whole** rather than only its tail, and clipping each rendered plan line (v2.9.1) | **Kept.** Fixes a real bug - `kMaxPopupChars` was documented as a bound on the string and implemented as a bound on the tail loop, so the body plus one long plan line could exceed it. The clip also makes the length a constant, which is what makes the character count in row 19 comparable between cooks. **Not** shown to fix a blank popup: the popup returned on a build compiled before this change. Enforces the rule above instead of explaining the failure. |
| Removing the `getInfoPopupString() called (...)` textport line (v2.9.1) | **Kept, at the user's request.** Its two facts live in row 19. Worth recording what it cost: printed from *inside* the callback, its absence and the popup's absence looked like a single symptom when they are opposite branches of the table above. A diagnostic that shares a code path with the thing it monitors will eventually be read as a cause. |

## License / third party

**FFTW3 is GPL.** The build vendors FFTW 3.3.11 (single precision, AVX2) as `libfftw3f-3.3.11-avx2.dll` and ships
it beside `FFT.dll`; distributing the plugin therefore falls under the GPL. `3rdParty/fftw3/VERSION` records the
source URL, the MD5 of the tarball, the SHA-256 of the built DLL, and the one upstream patch (upstream's CMake
build still stamps 3.3.10). If GPL is a problem for a project, the FFT backend is a swap: the `IFFTEngine`
interface and the registry in `FftBackend.h` exist for that.

**Intel oneMKL is not distributed with this project.** It is under the Intel Simplified Software License —
redistribution allowed, no royalty, but the DLLs may not be modified or renamed and the license notices have to
ship with them, which is the user's call and not a build script's. The `FFT Backend` toggle loads it from wherever
the user installs it; if it is not there the node logs which file it looked for and uses FFTW3. This is also why
neither library is *linked*: both export the same `fftwf_*` symbols, so a link would freeze the choice at build
time and make the toggle impossible.
