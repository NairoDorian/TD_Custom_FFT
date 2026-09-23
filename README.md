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
> `python dev/ci.py --project ../Plugin_FFT/PluginProjects/FFT`. Builder-side changes are in
> `../PluginBuilder_V2/CHANGELOG.md`.

---

## How to use this documentation

Six markdown files, six different jobs. Read the one that matches your question:

| File | What it is for | Read it when |
|---|---|---|
| **README.md** (this file) | What the node does, every parameter, building and testing, current performance, the diagnostics | You are using the node, or about to change something and want to know what it is for |
| [CHANGELOG.md](CHANGELOG.md) | Version history, newest first, with the A/B measurements behind each performance change and the optimisations that were measured and *not* kept | You want to know *when* something changed, or what a kernel change actually bought |
| [AUDIT_AND_PLAN.md](AUDIT_AND_PLAN.md) | The current audit and the open plan: where the time goes, what is still open and why rejected ideas were rejected | You want the to-do list and its acceptance criteria, or something to make faster |
| [ESSENTIATD_LESSONS.md](ESSENTIATD_LESSONS.md) | What is worth porting from EssentiaTD (features, cook-model gaps, test harness) | You are adding a feature or a robustness fix and want prior art |
| [INSTALLER_PLAN.md](INSTALLER_PLAN.md) | Plan for a per-user Windows installer: exact DLL set, prerequisite checks, licensing, build pipeline | You want to ship the plugin to another machine |
| [PluginProjects/FFT/3rdParty/fftw3/README.md](PluginProjects/FFT/3rdParty/fftw3/README.md) | The vendored FFTW build, the oneMKL alternative, and the license terms of both | You are rebuilding or replacing the FFT library |

Two conventions, so the numbers can be trusted:

- **Every performance figure names the machine it was measured on** (here, the i9-13900H). Times move
  with thermals and with the live FFT plan; the *direction* of a comparison is the durable part.
- **Say whether a statement is measured, derived, or a guess.** Claims that could not be verified are
  labelled as such.

---

## Where to change what

Every path is relative to `PluginProjects/FFT/`.

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

Two rules that are easy to break and hard to debug:

1. **A parameter that feeds a cached table must be added to that table's key**
   (`WindowKey` / `WarpKey` / `WeightKey` in `source/AnalysisPipeline.h`). Nothing enforces this at
   compile time, and forgetting it produces a table that is silently stale until the user changes
   that one parameter.
2. **Nothing that allocates, locks or prints may be added to the cook path.** The plugin's real-time
   argument is that a steady-state cook does none of the three; see *Performance* and *The cost of the
   info chain*.

And one convention for comments: **refer to code by symbol, not by line number** (`FFT::pollParameters()`,
not `source/FFT.cpp:239`). A symbol survives an edit; a line number does not. Add a file name only when the
symbol is ambiguous across files.

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
- **AVX2 / FMA** everywhere it pays: windowing, magnitude (rsqrt + Newton step, ~23-bit accuracy), warp
  interpolation (one unaligned load + `vpermps` per tap where the 8 output bins' taps fit one 8-float window,
  `vgatherdps` elsewhere), weighting fused with the dB reference peak, a table-free `FastLog2Seg` `20·log10` for
  dB (no gather, 0.00016 dB max error), in-place ballistics, a branch-free 4-chain peak search, and vectorised
  spectral features.
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
- **dB modes** with **dB Reference**: Frame Peak (0 dB = loudest bin), 0 dBFS (absolute), Slow AGC.
- **Magnitude normalization**: Coherent Gain (`mean(window) = 1`) or Full Scale (sine amplitude 1 → 1.0).
- **Window length** in samples or in **milliseconds** (sample-rate independent).
- **Ballistics** as per-frame coefficients or in **milliseconds** (frame-rate independent, uses `OP_TimeInfo`).
- **Async analysis with a hard real-time cook thread**: the FFT runs on a worker; the cook thread only ingests and copies.
  Job and result handoff are wait-free triple buffers (`AsyncAnalysis`); with `Worker Wake = Poll` (default) the
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
├── tests/dsp_tests.cpp   <-- headless golden-vector tests (748 checks, incl. lock-free handoff stress)
├── bench/bench.cpp       <-- per-stage benchmark + cook-thread simulation (--cook N, --info N, --backend fftw3|mkl, --gate)
├── bench/perf_baseline.json <-- baseline for the perf-regression gate (`ctest -L perf`)
├── bench/fftw_threads_probe.cpp <-- measures whether FFTW's built-in threading helps (it does not)
├── bench/fftw_version_probe.cpp <-- reports the vendored library's version and available flags
└── 3rdParty/fftw3/       <-- vendored FFTW 3.3.11 AVX2 (VERSION record, header, .def/.lib, runtime DLL)
```

A working copy may also have an `FFT_REFERENCE/` folder at the repository root. It is local-only
(git-ignored); nothing in the build, the tests or this documentation depends on it.

`DSPModules.h`, `FftBackend.h` and `RateModel.h` are header-only by design and are included by more than one
translation unit (the node, the tests and the bench), so their free functions are `inline`. Before moving a
function out of one into a `.cpp`, check who else includes it: the tests and the bench link against these
headers directly and would otherwise fail at link time, not at compile time.

## Building

### From TouchDesigner (PluginBuilder_V2)
Drop `PluginBuilder.tox` into `Plugin_FFT.toe`, type `FFT` as the plugin name — PluginBuilder finds the
existing project (via this folder's `plugin.json`), configures, compiles on every source save and
hot-reloads the DLL (rename-in-place, no unload gap). The loader path, `plugin.json` manifest,
`__Plugins__/FFT/` deploy folder and the CMake functions (`td_add_plugin`, `td_plugin_use_fftw3 ... DYNAMIC`,
`td_plugin_optimize`) all come from the sibling [PluginBuilder_V2](../PluginBuilder_V2) repo; `CMakeLists.txt`
resolves it as `../../../PluginBuilder_V2` (override with `-DPLUGIN_BUILDER_DIR=`). Headless check after a
builder upgrade: `python ../PluginBuilder_V2/dev/ci.py --project PluginProjects/FFT`.

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

`fft_tests` prints a check count as well as a pass/fail; the baseline is **748 checks, 0 failures**. Treat the
count as a signal: a refactor that removes a check is as suspicious as one that fails one, so compare the
number, not just the exit code. The oneMKL and from-wisdom test branches are machine-dependent - a different
total on another machine is not a regression, but a different total on this one is.

A standalone build deploys `FFT.dll` + `libfftw3f-3.3.11-avx2.dll` into `__Plugins__/FFT/` (rename-in-place).
The oneMKL DLLs are never deployed by the build: they are optional and the user's to install (the plugin only
*looks* for them, in its own directory). The deployment set is 14 `.3.dll` files, ~456 MiB: `mkl_rt`,
`mkl_core`, `mkl_sequential`, the kernel set `mkl_def` / `mkl_mc3` / `mkl_avx2` / `mkl_avx512` / `mkl_avx10` and the
VML set `mkl_vml_def` / `mkl_vml_mc3` / `mkl_vml_avx2` / `mkl_vml_avx512` / `mkl_vml_avx10` / `mkl_vml_cmpt`, plus the
`oneMKL-licenses/` folder. `mkl_intel_thread`, `mkl_tbb_thread` and `libimalloc.dll` are **not** needed: the plugin
forces oneMKL's sequential threading layer at load. Only five of them load on a given CPU (on the i9-13900H:
`mkl_rt`, `mkl_core`, `mkl_sequential`, `mkl_avx2`, `mkl_vml_avx2`, ~177 MiB); the other kernel variants are there
for AVX-512 and older CPUs. `.gitignore` keeps `__Plugins__/FFT/` out of the repository, so a fresh clone builds
and runs on FFTW3 alone; only the `FFT Backend` toggle needs oneMKL. Details in
[`3rdParty/fftw3/README.md`](PluginProjects/FFT/3rdParty/fftw3/README.md#using-intel-onemkl-instead-the-fft-backend-toggle).

Requirements: Windows 10/11 x64, Visual Studio 2022/2026 C++ tools, CMake ≥ 3.21, Ninja, a CPU with AVX2 + FMA.

## Parameters

In dialog order (the order `setup()` in `source/Parameters.cpp` registers them). Greying out is live: the node
tells TouchDesigner which parameters the current settings make inert (`setEnableStates()`).

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
| Spectrum | Warp Aggregation | Menu | **Peak** | What an output bin reports when it covers several FFT bins (the coarse, usually high-frequency, part of a perceptual axis): Off = interpolate between two FFT bins (a narrow peak between the taps is skipped) · Peak = the largest FFT bin in the range (no peak is ever dropped) · RMS = the power mean of the range (energy-faithful). Only those bins are affected; Info CHOP `aggregated_bins` counts them |
| Spectrum | Log Floor Hz | Float | 20 | lowest frequency of the Log / Melog grid |
| Spectrum | Window Length Mode | Menu | Samples | Samples or Milliseconds |
| Spectrum | Window Sampling | Int | 3175 | analysis window in samples (= 72 ms @ 44.1 kHz; slider 1…32768, hard limit 65536) |
| Spectrum | Window Length ms | Float | 72 | used when mode = Milliseconds (slider 1…1000, accepted 0.1…5000) |
| Spectrum | Zero-Padding | Toggle | **On** | Off = the FFT runs on the window itself (N = window length rounded up to even, so the last bin is exactly Nyquist); Zero-Pad Len is greyed out. A power-of-two window is the fast case (the default 3175 → 3176 = 8·397 plans a slower FFT) |
| Spectrum | Zero-Pad Len | Menu | 16384 | FFT size with Zero-Padding on (1K…64K; auto-grown to ≥ next pow2 of the window). Overridden by a Quality Preset |
| Spectrum | FFT Planner | Menu | Auto | Auto: instant plan now, measured plan upgraded in the background (wisdom-cached) · Fast (Estimate only) · Measured (blocking, once per size) · **Patient**: like Auto but the background upgrade is `FFTW_PATIENT`, which executes faster than a MEASURE plan (9.70 against 11.55 µs at N = 16384, i9-13900H). The PATIENT search has no time limit: about 2.7 s of above-normal-priority planning at N = 32768 on the i9-13900H, longer on slower machines, once per size per machine and never on a TouchDesigner thread (see *Threading*); the resulting wisdom is also used by Auto. Only the FFTW3 backend honours the policy |
| Spectrum | Input Ingest | Menu | Auto | Auto = only the samples that are new since the last cook, from the input's start index and cook count (an overlapping or re-delivered buffer is not appended twice) · Append All = the newest block, every cook |
| EQ | EQ Enable | Toggle | **Off** | Off = no EQ code and no EQ parameter reads at all |
| EQ | High Shelf / Low Shelf | Toggle | On / On | per-shelf bypass (only read when EQ Enable is on) |
| EQ | High Boost dB, High Cutoff Hz, Low Boost dB, Low Cutoff Hz, EQ Q Factor, EQ Blend Amount | Float | 6 / 1000 / 0 / 200 / 0.707 / 1 | RBJ shelving EQ applied at ingest to new samples (stateful) |
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
| Performance | Async Analysis (worker thread) | Toggle | **On** | FFT & post-processing on a worker thread; the cook only ingests and copies (its cost scales with the output bin count; measure with `fft_bench --cook`). Off = inline, and **one thread for the whole node**: no worker, and no background plan measurement either |
| Performance | FFT Backend (off: FFTW3 / on: Intel oneMKL) | Toggle | **Off** | Off = the vendored FFTW3 3.3.11 AVX2 build. On = Intel oneMKL's FFTW3 interface (`mkl_rt.3.dll`), which runs the FFT 18–27 % faster on the i9-13900H (see *FFTW3 vs Intel oneMKL*) but has to be installed by the user. Missing DLL = logs and falls back to FFTW3, never a planless node. A toggle rather than a menu because there are two libraries; the registry in `FftBackend.h` and the `static_assert`s in `Parameters.cpp` are what a third would extend |
| Performance | Worker Wake | Menu | Poll | Poll = the worker checks the job slot every 2 ms (no kernel call on the cook thread; pickup 0–2 ms) · Signal = the cook wakes it every cook (one `SetEvent`, ~5–25 µs on the cook thread; pickup ~0.02 ms). The worker goes dormant after 500 ms idle either way. Greyed out with Async off |
| Performance | Worker Priority | Menu | Highest | Highest = `THREAD_PRIORITY_HIGHEST` · MMCSS Pro Audio = registered with the Multimedia Class Scheduler, which schedules it ahead of normal threads and exempts it from power throttling (for 120+ fps projects on a busy machine). Applied at thread start; greyed out with Async off |
| Performance | Spectral Features (Info CHOP) | Toggle | Off | On = eight `feature_*` Info CHOP channels (centroid, rolloff, flatness, flux, RMS dB, bass / mid / high dB), computed on the worker from the linear magnitude. ~3.5 µs per analysis at 8193 bins (i9-13900H) |

With the defaults (Coherent Gain, Frame Peak, Samples, Coefficient, Kaiser β 15 Manual; EQ and Ballistics
**off**) the node outputs N/2+1 of the padded FFT (8193 samples at the default pad) and output bins that cover
several FFT bins report their peak. Every optional section is bypassed entirely — code *and* parameter reads —
when disabled: `eval()` reads **30** parameters per cook in the default configuration, against **43** with every
optional section on (EQ with both shelves, a dB mode, ballistics; Kaiser Beta Mode = Manual). The live count is
reported as the Info CHOP channel `param_reads`, so these numbers can be checked against a running node. Every
parameter is read on every cook, so a change takes effect on the next frame. When PluginBuilder reloads the DLL,
a node keeps the parameter values it already had; parameters it did not have yet arrive with their defaults.

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
   interpolation, or a 4-tap Catmull-Rom cubic with `Warp Interpolation = Cubic`. When the taps of 8 output bins all
   fall inside one 8-float window (the fine, upsampled part of the axis — ~83 % of the vectors at a 16384-bin Log
   axis), each tap is read with one unaligned load plus one `vpermps`; the rest use an AVX2 `vgatherdps`. Both
   paths are bit-identical.
4. **Aggregation.** Where one output bin covers two or more FFT bins (the coarse end of a perceptual axis),
   `Warp Aggregation` replaces the interpolated value with the largest FFT bin in the range (Peak, the default) or
   its power mean (RMS); Off keeps the interpolation. The trailing run of aggregated bins is not interpolated
   first, since aggregation overwrites it anyway.

So going from 16384 bins to 16384 bins does **not** upsample anything: the same band is described by a different
number of samples. Fewer bins means the axis is coarser (each output bin covers a wider slice of the linear grid,
and `Warp Aggregation` decides what it reports); more bins than `fft_size/2+1` means bins are interpolated
*between* real FFT bins — smooth, but with no information that was not already there. The one case with no loss is
the identity grid, where `isIdentity()` is true and the warp is a `memcpy`: the output is the linear FFT grid
itself, copied, with no interpolation and no rounding. There are two ways to get it:

- **`Raw RFFT Bins` on.** Forces it: N/2+1 samples, DC..Nyquist, whatever the axis parameters say — and it greys
  those parameters out, so the two spellings cannot disagree. This is the one to use.
- **The equivalent setting**: Scale = Linear, `blend = 0`, `Display Max >= Nyquist` and `n_out = fft_size/2+1`
  (Output Bins Mode = Auto, or Fixed with that count). The warp detects it on its own.

### Output sample rate

The CHOP reports **`output_sample_rate = output bins × me.time.rate`**: one output vector of `bins`
samples is produced every `1/me.time.rate` seconds, so at the defaults (Output Bins Mode = Auto, Zero-Pad
Len 16384 → 8193 bins, 60 fps) that is **491 580 samples/s** (983 040 with `Fixed` and 16384 bins). `me.time.rate`
is the timeline rate *where the node lives* (`OP_TimeInfo::rate`), so inside a component with Component Time the
component's rate is used, not the root's; it is read every cook, so an FPS change shows up on the next frame.

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

`getInfoPopupString` (middle-click the node) prints the rate, the axis and the measured throughput, plus the
cook delta the throughput was measured over.

## Performance

All figures below: **i9-13900H** (Raptor Lake, 6 P-cores + 8 E-cores / 20 threads, AVX2 + FMA, no AVX-512),
`fft_bench`, 1 channel, medians in µs of runs interleaved in random order, pinned to one P-core at high priority
on an idle CPU. FFTW3 backend with a measured plan unless the row says otherwise.

| Metric | µs (median) |
|---|---|
| pipeline, full chain (dB + ballistics + spectral features) | **22.20** |
| pipeline, plugin defaults | **17.60** |
| pipeline, Visual 60 preset | **8.90** |
| cook thread, Async on (Worker Wake = Poll), plugin defaults | **0.60** |
| cook, synchronous (Async off) | **25.20** |
| stage: warp, Log linear, 16384 bins | 2.52 |
| stage: warp, Log cubic, 16384 bins | 4.82 |
| stage: dB normalised, 16384 bins | 3.58 |
| stage: peak + index, 16384 bins | 0.94 |
| spectral features, 8193 bins | ~3.5 |

The FFT itself is most of the default cost — roughly two thirds, derived from two separate runs (11.55 µs FFTW3
FFT at N = 16384, below, against the 17.60 µs default pipeline) — so the levers that matter are `Zero-Pad Len`
and `FFT Backend`. With Async on (the default) none of this runs on the cook thread: the cook ingests, publishes
the job and copies the last result — 0.60 µs at the defaults (8193 output bins), a cost that scales with
the output bin count.

### FFTW3 vs Intel oneMKL

FFT stage only, median of 3 pinned runs:

| N | FFTW3 3.3.11 AVX2, `FFTW_MEASURE` | FFTW3, `FFTW_PATIENT` | Intel oneMKL 2026.1.0 | oneMKL faster than MEASURE by |
|---|---|---|---|---|
| 8192 | 4.71 µs | – | 3.87 µs | 18 % |
| 16384 | 11.55 µs | 9.70 µs | 8.41 µs | 27 % |
| 32768 | 23.65 µs | – | 17.73 µs | 25 % |

A PATIENT plan narrows the gap without closing it (oneMKL still leads by ~13 % at 16K). FFTW3 stays the default
because it is 3 MB, vendored and wisdom-cached; oneMKL loads 5 DLLs (~177 MiB on this CPU) and spends ~39 ms
initialising once per process (on the worker thread when Async is on). Deployment notes, the OpenMP hazard it
avoids and the DLL list are in [`3rdParty/fftw3/README.md`](PluginProjects/FFT/3rdParty/fftw3/README.md#using-intel-onemkl-instead-the-fft-backend-toggle).

### Measuring

- **Per-stage timings**: `fft_bench --channels N` (see *Building > Standalone* for the full invocations);
  `--cook N` simulates N cooks with Async on and off and reports the cook-thread cost and worker pickup;
  `--info N` measures the info-callback access cost (about 0.6 µs per cook).
- **Library A/B**: `--backend fftw3` or `--backend mkl`. The flag takes a **name**; a number such as
  `--backend 1` is rejected and the run falls back to FFTW3 with a single "unknown --backend" line that is easy
  to miss.
- **Interleave every A/B.** Each `fft_bench` process pays a cold first-plan and cold-cache cost on the library it
  loads, so one run of A followed by one run of B favours whichever ran second. Alternate the two within one loop,
  pin to one core, and take several pairs before believing a difference.
- **Perf gate**: `ctest -L perf` runs `fft_bench --gate bench/perf_baseline.json` and fails on a regression
  against the baseline; `--gate ... --update 1` rewrites the baseline.
- The per-kernel A/B history, and the optimisations that were measured and not kept, are in `CHANGELOG.md`.

### Threading

Two threads, both named for the debugger, **neither below normal priority**:

| Thread | Priority | Runs |
|---|---|---|
| analysis worker (`FFT Custom CHOP analysis`) | `THREAD_PRIORITY_HIGHEST` (default), or MMCSS "Pro Audio" with `Worker Priority = MMCSS` | window → FFT → magnitude → warp → weighting → dB → ballistics → peak (→ spectral features) |
| FFTW background planner (`FFT background planner`) | `THREAD_PRIORITY_ABOVE_NORMAL` | one `FFTW_MEASURE`/`FFTW_PATIENT` plan per FFT size, then exits |

The worker sits one notch above the planner so a cook always wins the core back from it; the planner is above
normal so a measurement is not starved under load. A `FFTW_PATIENT` measurement has **no time limit**: it runs off
the cook thread, so a slower machine takes as long as it needs to find the best plan (~2.7 s at N = 32768 on the
i9-13900H), once per size per machine, then wisdom caches it. FFTW's planner is process-wide, so a re-plan
requested meanwhile (a size change, another node) waits for the planner lock until the measurement ends: with
Async on that waiter is the analysis worker and TouchDesigner keeps cooking (the node holds its last spectrum);
with Async off it is the cook thread. Deleting the node or quitting TouchDesigner mid-measurement waits for it
too. A measurement that is no longer wanted (the size or backend changed) is handed to a graveyard and reaped
when it finishes, rather than blocking a cook.

**FFTW's planner is serialised, on purpose.** The FFTW manual states that `fftwf_execute` (and the new-array
variants) are the *only* thread-safe routines; planner calls share wisdom and trigonometric tables and "should only
be called from one thread at a time". So the engine holds one process-wide mutex around every planner call,
`destroy_plan` and wisdom import/export, on every thread. That is FFTW's own recommended pattern, and it is what
makes the Async worker and the background planner legal. `fftwf_make_planner_thread_safe` is deliberately not
used: the manual calls it "the worst of all worlds", and the mutex already does its job with our own priority
ordering.

**`Async` off means one thread for the whole node.** No worker is started, and no background plan measurement
either: `prepare()` leaves the node on `FFTW_ESTIMATE`, says so in the plan line ("measured upgrade deferred:
Async is off"), and remembers that it is owed one, so turning `Async` back on starts the measurement then. A
measurement already running when Async is switched off finishes first. `fft_tests` asserts both halves.

**FFTW's own threading does not help this node.** FFTW parallelises the `howmany` loop and multi-dimensional
transforms, not the inside of a single 1-D transform, and `Mono Mix` (the default) issues exactly one transform per
cook. The vendored 3.3.11 build is configured without threads, which costs nothing (the plugin never calls them).
`fft_threads_probe` (`bench/fftw_threads_probe.cpp`) measures it on a DLL that has the threads API and reports
"absent" on one that does not.

**Where the parallelism is.** This node is one mono channel per instance; several channels means several node
instances, each with its own worker, and that is what spreads across cores. None of the five `Performance`
controls sets a thread count. Inside one node, the channel loop runs under `std::execution::par` when a job has
more than one channel (`Channels = All Channels`), with no knob: each channel owns its `DspState` and the only
shared state is read-only. With `Mono Mix` the branch is a single integer compare and the serial path is taken.
`fft_bench --channels 2|4|8` measures the fan-out; the output is identical to the serial path bin for bin.

## The middle-click info popup and the diagnostics

### The popup is a snapshot of the last cook, not a query

TouchDesigner's middle-click does **not** call the plugin; it renders the text the *last cook* left behind. Every
information callback runs inside a cook, in this order (documented at the top of `CHOP_CPlusPlusBase.h`):

```
getGeneralInfo -> getOutputInfo -> getChannelName xN -> execute()
  -> getNumInfoCHOPChans -> getInfoCHOPChan xN
  -> getInfoDATSize -> getInfoDATEntries xN
  -> getInfoPopupString -> getWarningString -> getErrorString
```

Two consequences:

1. **A node that is not cooking has no information surface at all**: no popup, no Info CHOP, no Info DAT, no
   warning and no `getErrorString`, so a hard failure (a plan that will not build, an input with no usable
   sample rate) shows as silence instead of an error badge.
2. **No plugin-side change can force a render.** If the chain is not entered, nothing in `getInfoPopupString`
   is read.

### What the popup contains

`renderInfoCache()` formats the Info DAT rows and the popup text at most every 250 ms, or when the status or the
plan log changes; `getInfoPopupString` hands the text over with one `setString`. The text starts with an identity
block — `Node:` (the node path), `Plugin:` (the version) and `Binary:` (the DLL that answered,
`OP_NodeInfo::pluginPath`) — so it is never textually empty. Then come the engine, FFT size, axis, rate, output,
cook and DSP times, SIMD, and the last three plan-log lines, each clipped to 72 characters (`kMaxTailLineChars`).
The whole string is bounded at **1200 characters** (`kMaxPopupChars`): every line after the identity block goes
through one `add` path that refuses a line that would not fit. A given node therefore hands over the same length
every cook; the length changes only when an input changes (node path, install path, a plan line). The Info DAT
`plan_log_*` rows and the textport carry the plan lines unclipped.

**Rule: never put a diagnostic in the popup string.** A popup that grew to ~1760 characters rendered empty, and
shrinking it brought it back. The SDK documents no length limit for `OP_String::setString`, so the mechanism is
not established; the bound and this rule are the safeguard. Diagnostic values go in the Info DAT row
`info_callback_calls`, which TouchDesigner reads as a value, not as the popup.

### Diagnosing a blank popup

Read Info DAT row 19, `info_callback_calls`:
`popup N (M chars), Info CHOP N, Info DAT N | largest cook gap X ms` (refreshed with the rest of the cache).

| | Chain **not** entered | Chain entered, text **not** rendered |
|---|---|---|
| The call counters | **frozen** (they stop advancing) | **climb** — the popup count tracks the cook count |
| The popup's character count | stale, alongside the frozen counters | steady and ordinary (a constant for a given node) |
| Meaning | the node is not cooking → fix the cook, not the popup | the string was handed over and TouchDesigner did not draw it → **not a plugin problem** |
| Fix | the checklist below | report to Derivative with that row as evidence |

Nothing about this is printed to the textport: a print on the info path costs cook time and shares a code path
with the thing it would be monitoring.

**Making sure the node is cooking**, most frequent cause first:

1. **Is the loaded DLL the one you just built?** TouchDesigner does **not** reload a DLL when the file on disk
   changes. Pulse **`Reloadplugin`** on the PluginBuilder COMP (it calls `_do_copy_plugin(force=True)` in
   `PluginBuilderExt.py`), or turn the loader's `unloadplugin` off and on. While the loader is unloaded the popup
   is blank, even though the spectrum on screen can still look fine. To confirm which DLL answered, middle-click
   and read the `Binary:` line.
2. **There are two installs, and only one auto-updates.** `<project>/__Plugins__/FFT/FFT.dll` is copied by
   PluginBuilder on every build. `~/Documents/Derivative/Plugins/FFT/FFT.dll` (the registered Custom Operator) is
   updated **only** by the **Install Plugin** pulse and drifts stale. Each install also resolves FFTW3/oneMKL from
   *its own* directory, so a measurement on one says nothing about the other.
3. **Is the timeline advancing?** The node sets `cookEveryFrame = true`, so it cooks on every *timeline* frame. A
   paused timeline stops cooking, the spectrum holds its last frame and *looks* fine, and the popup goes blank.
   This is the best explanation for "it worked a minute ago".
4. **Is an error or warning showing on the node?** TouchDesigner shows an error state **in place of** the operator
   information. An exception error clears on the next cook that completes, so a persistent error is a current
   fault.
5. **The viewer flag on the loader.** For a `.dll` hosted in the built-in CPlusPlus CHOP, the SDK's
   `OP_CustomOPInfo::cookOnStart` does not apply (it is Custom-Operator only), so an active **viewer** flag is the
   only kick-start. PluginBuilder sets it (`_ensure_loader_live()`). `PluginBuilderExt.py` is loaded when the COMP
   is created, so a change there is inert until the extension is re-initialised or the `.toe` is reopened.

Not established, and not observable from inside a plugin: why a longer string renders blank, and whether
TouchDesigner's own popup query re-queries, caches or times out. Establish which column of the table applies
before changing code.

### The cost of the info chain

The callbacks run inside the cook, on the thread that just ran the analysis, and TouchDesigner drives them one
item at a time (36 `getInfoCHOPChan` calls, one `getInfoDATEntries` call per row). So they only hand over cached
strings: the status snapshot is memoized behind a version stamp, the plan log exposes `version()` and
`snapshotTail(n, out)` so the popup copies only the three lines it renders into a reused vector, and the
formatting in `renderInfoCache()` reuses each string's capacity. In the steady state the chain takes no lock and
(almost) no allocation. `fft_bench --info 2000` drives the same access pattern against the real `PlanLog`:
about **0.6 µs per cook** on the i9-13900H. Re-take that number after touching any info callback.

## License / third party

**FFTW3 is GPL.** The build vendors FFTW 3.3.11 (single precision, AVX2) as `libfftw3f-3.3.11-avx2.dll` and ships
it beside `FFT.dll`; distributing the plugin therefore falls under the GPL. `3rdParty/fftw3/VERSION` records the
source URL, the MD5 of the tarball, the SHA-256 of the built DLL, and the one upstream patch (upstream's CMake
build still stamps 3.3.10). If GPL is a problem for a project, the FFT backend is a swap: the `IFFTEngine`
interface and the registry in `FftBackend.h` exist for that.

**Intel oneMKL is optional and not distributed with this project** (14 DLLs, ~456 MiB; see *Building >
Standalone*). It is under the Intel Simplified Software License — redistribution allowed, no royalty, but the
DLLs may not be modified or renamed and the license notices have to ship with them, which is the user's call and
not a build script's. The `FFT Backend` toggle loads it from wherever the user installs it; if it is not there
the node logs which file it looked for and uses FFTW3. This is also why neither library is *linked*: both export
the same `fftwf_*` symbols, so a link would freeze the choice at build time and make the toggle impossible.

**The repository has no `LICENSE` file yet.** What it needs before the plugin is distributed is listed in
[INSTALLER_PLAN.md](INSTALLER_PLAN.md), §6.
