# TouchDesigner Custom FFT CHOP (`Plugin_FFT`)

A real-time C++ CHOP for **Derivative TouchDesigner** (C++ API 10, TouchDesigner 2025.30000+) that turns
audio into a psychoacoustically scaled magnitude spectrum: FFTW3 single-precision R2C transform,
256-bit AVX2/FMA post-processing, Log/Mel/ERB/Bark/Chroma re-mapping, equal-loudness weighting,
dB conversion with selectable reference, and attack/release ballistics — all with zero allocations
in the cook loop.

Built with [PluginBuilder_V2](../PluginBuilder_V2) (hot reload from TouchDesigner) but also builds
standalone with CMake + Ninja and ships headless tests and a per-stage benchmark.

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
- **AVX2 / FMA** everywhere it pays: windowing, magnitude (rsqrt + Newton step, 2.2e-7 rel. error),
  warp interpolation (`vgatherdps`), weighting, single-gather 2048-entry LUT `20·log10` (0.002 dB error), ballistics, peak search.
- **Psychoacoustic scales**: Logarithmic, Mel, ERB, Bark, Chroma, Linear, Mel+Log blend, with a `Warp Blend`
  slider and an identity (memcpy) bypass when the grid is exactly linear. The linear grid — every FFT bin
  bit-for-bit, no interpolation, maximum precision — is a setting, not a mode: `Scale = Linear`,
  `Warp Blend = 0`, `Display Max >= Nyquist`, `Output Bins = fft_size/2+1`. See
  [How the resampling works](#how-the-resampling-works).
- **Equal-loudness weighting**: A (IEC 61672), C, ITU-R 468.
- **dB modes** with **dB Reference**: Frame Peak (legacy, 0 dB = loudest bin), 0 dBFS (absolute), Slow AGC.
- **Magnitude normalization**: Coherent Gain (legacy, `mean(window) = 1`) or Full Scale (sine amplitude 1 → 1.0).
- **Window length** in samples (legacy) or in **milliseconds** (sample-rate independent).
- **Ballistics** as per-frame coefficients (legacy) or in **milliseconds** (frame-rate independent, uses `OP_TimeInfo`).
- **Async analysis with a hard real-time cook thread**: the FFT runs on a worker; the cook thread only ingests and copies.
  Job and result handoff are wait-free triple buffers, the worker polls on a high-resolution timer (no kernel wake-up
  per cook) — no mutex, no syscall, no allocation on the cook thread after warm-up (v2.4).
- **Diagnostics**: Info CHOP (`cook_time_us`, `dsp_time_us`, `peak_freq_hz`, `hold_frames`, `jobs_dropped`, …), Info DAT (plan log, wisdom path,
  window resolution), middle-click popup, warning/error strings, AVX2 CPU guard (no illegal-instruction crash).
- Textport logging through `PySys_WriteStdout` (no Python script injection).

## Project layout

```text
PluginProjects/FFT/
├── CMakeLists.txt        <-- 15 lines: include(PluginBuilder_V2/cmake/TDPlugin.cmake) + td_add_plugin(...)
├── plugin.json           <-- manifest read by PluginBuilder (family, optype, deps)
├── source/
│   ├── DSPModules.h      <-- TouchDesigner-independent DSP (FIFO, EQ, window, warp, weighting, dB, ballistics, FFTW engine)
│   ├── FftBackend.h      <-- FFT library registry + runtime loader (FFTW3 / oneMKL, no import library)
│   ├── FFT.h / FFT.cpp   <-- the CHOP operator (API 10 entry points, per-channel pipeline, telemetry)
│   └── Parameters.h/.cpp <-- typed parameter definitions (enum classes, single eval() per cook)
├── tests/dsp_tests.cpp   <-- headless golden-vector tests (607 checks, incl. lock-free handoff stress)
├── bench/bench.cpp       <-- per-stage benchmark + cook-thread simulation (--cook N, --info N, --backend fftw3|mkl)
├── bench/fftw_threads_probe.cpp <-- measures whether FFTW's built-in threading helps (it does not)
└── 3rdParty/fftw3/       <-- vendored FFTW 3.3.11 AVX2 (VERSION record, header, .def/.lib, runtime DLL)
```

## Building

### From TouchDesigner (PluginBuilder_V2)
Drop `PluginBuilder.tox` into `Plugin_FFT.toe`, type `FFT` as the plugin name — PluginBuilder finds the
existing project, configures, compiles on every source save and hot-reloads the DLL (rename-in-place, no unload gap).

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
`PLUGIN_BUILDER_DIR` defaults to the sibling `../../../PluginBuilder_V2`; pass `-DPLUGIN_BUILDER_DIR=` otherwise.
A standalone build deploys `FFT.dll` + `libfftw3f-3.3.11-avx2.dll` into `__Plugins__/FFT/` (rename-in-place).
The oneMKL DLLs are never deployed by the build: they are 521 MB, entirely optional, and the user's to install
(the plugin only *looks* for them, in its own directory). On this machine they are installed in `__Plugins__/FFT/`
alongside the notices, which `.gitignore` keeps out of the repository — so a fresh clone builds and runs on FFTW3
alone, and the `FFT Backend` toggle is the only thing that needs them.

Requirements: Windows 10/11 x64, Visual Studio 2022/2026 C++ tools, CMake ≥ 3.21, Ninja, a CPU with AVX2 + FMA.

## Parameters

| Page | Parameter | Type | Default | Notes |
|---|---|---|---|---|
| Spectrum | Channels | Menu | **Mono Mix** | Mono Mix (average all inputs → 1 analysis channel) / First Channel / All Channels (one FFT per channel) |
| Spectrum | Scale | Menu | Log | Log / Mel / ERB / Bark / Chroma / Linear / Melog |
| Spectrum | Warp Interpolation | Menu | Linear | Linear (2 taps) / Cubic Catmull-Rom (4 taps; a 16K FFT + cubic looks like 32K + linear at half the cost) |
| Spectrum | Display Max Hz | Float | 24000 | clamped to Nyquist; slider max 192000 so the full band is reachable at any input rate |
| Spectrum | Output Bins | Int | 16384 | size of the warped output (hard-clamped 8…262144; slider max 65536, so `fft_size/2+1` is reachable at every pad size) |
| Spectrum | Warp Blend | Float | 0.963 | 0 = linear grid, 1 = fully perceptual |
| Spectrum | Log Floor Hz | Float | 20 | lowest frequency of the Log / Melog grid |
| Spectrum | Window Length Mode | Menu | Samples | Samples (legacy) or Milliseconds |
| Spectrum | Window Sampling | Int | 3175 | analysis window in samples (= 72 ms @ 44.1 kHz) |
| Spectrum | Window Length ms | Float | 72 | used when mode = Milliseconds |
| Spectrum | Zero-Pad Len | Menu | 32768 | FFT size (auto-grown to ≥ next pow2 of the window) |
| Spectrum | FFT Planner | Menu | Auto | Auto: instant plan now, measured plan upgraded in the background (wisdom-cached) · Fast (Estimate only) · Measured (blocking, once per size) · **Patient**: like Auto but the background upgrade is `FFTW_PATIENT` (−12 % FFT time at N = 32768; ~3 s of above-normal-priority planning once per size per machine, never on a TouchDesigner thread; the resulting wisdom is also used by Auto) |
| EQ | EQ Enable | Toggle | **Off** | Off = no EQ code and no EQ parameter reads at all |
| EQ | High Shelf / Low Shelf | Toggle | On / On | per-shelf bypass (only read when EQ Enable is on) |
| EQ | High/Low Boost dB, Cutoff Hz, Q, Blend | Float | 6 / 1000 / 0 / 200 / 0.707 / 1 | RBJ shelving EQ applied at ingest to new samples (stateful, 3.6 µs/channel) |
| Window & Weighting | Window Type | Menu | Kaiser | Kaiser / Hann / Hamming / Blackman / Blackman-Harris / Rectangular |
| Window & Weighting | Kaiser Beta | Float | 15 | |
| Window & Weighting | Loudness Weighting | Menu | Off | Off / A / C / ITU-R 468 |
| Window & Weighting | Magnitude Normalization | Menu | Coherent Gain | Full Scale makes a sine of amplitude 1 read 1.0 |
| Loudness & Ballistics | Loudness Mode | Menu | Off | Off (linear) / dB / dB normalized 0…1 |
| Loudness & Ballistics | dB Reference | Menu | Frame Peak | Frame Peak / 0 dBFS / Slow AGC |
| Loudness & Ballistics | dB Range Floor | Float | 80 | |
| Loudness & Ballistics | Ballistics Enable | Toggle | **Off** | Off = no ballistics code and no attack/release parameter reads |
| Loudness & Ballistics | Ballistics Mode | Menu | Coefficient | Coefficient (per frame) or Milliseconds |
| Loudness & Ballistics | Attack / Release Speed | Float | 0 / 0 | per-frame coefficients 0…0.99 |
| Loudness & Ballistics | Attack / Release ms | Float | 50 / 200 | used when mode = Milliseconds |
| Loudness & Ballistics | Reset | Pulse | | clears ballistics, AGC and EQ state |
| Performance | Async Analysis | Toggle | **On** | FFT & post-processing on a worker thread; the cook only ingests and copies (≈ 11 µs at 16384 bins, 7 µs at 4096, measured). Off = inline, and **one thread for the whole node**: no worker, and no background plan measurement either |
| Performance | FFT Backend | Toggle | **Off** | Off = the vendored FFTW3 3.3.11 AVX2 build. On = Intel oneMKL's FFTW3 interface (`mkl_rt.3.dll`), which is 13–34 % faster on this CPU but has to be installed by the user. Missing DLL = logs and falls back to FFTW3, never a planless node. A toggle rather than a menu because there are two libraries; the registry in `FftBackend.h` and the `static_assert`s in `Parameters.cpp` are what a third would extend |

Defaults (Coherent Gain, Frame Peak, Samples, Coefficient; EQ and Ballistics **off**) reproduce the spectrum of the
early builds, in which the EQ was inactive. Every optional section is bypassed entirely — code *and* parameter
reads — when disabled: `eval()` reads **20** parameters per cook in the default configuration, against **33** with
every optional section on (EQ, dB, weighting, ballistics). The count is not an estimate: `eval()` increments it and
hands it back, and it is reported live as the Info CHOP channel `param_reads`, so the number in this paragraph can
be checked against a running node. Every parameter is
read on every cook, so a change takes effect on the next frame.

## How the resampling works

The FFT produces a fixed linear grid — `fft_size/2 + 1` bins, DC to Nyquist, every bin `sr_in/fft_size` Hz apart
(43.07 Hz at 44.1 kHz and N = 1024; 1.35 Hz at N = 32768). `Output Bins` says how many samples *describe that same
spectrum*, so the node resamples the linear grid onto a new frequency axis:

1. **Target axis.** For each output bin `i` of `n_out`, `computeTargetHzGrid` picks the frequency it should
   represent: `target_hz[i] = (1 - blend) * i/(n_out-1) * fmax + blend * perceptual(i)`, where `perceptual()` is the
   chosen scale (Log / Mel / ERB / Bark / Chroma) mapped from `log_floor` to `fmax`, and `fmax = min(Display Max,
   Nyquist)`. With `blend = 0` (or Scale = Linear) the axis is uniform: `n_out` bins evenly covering 0…fmax.
2. **Gather tables.** `buildWarpTables` converts each target frequency into a fractional position in the *linear*
   grid: `frac = target_hz[i] / nyquist * (fft_size/2)`, then stores `i0 = floor(frac)` and `w = frac - i0` into two
   tables (8 bytes per output bin).
3. **Interpolation.** `applyWarp` walks the output and reads `src[i0] + w * (src[i0+1] - src[i0])` — a 2-tap linear
   gather, 8 bins per AVX2 `vgatherdps`, or a 4-tap Catmull-Rom cubic with `Warp Interpolation = Cubic`.

So going from 16384 bins to 16384 bins does **not** upsample anything: the same band is described by a different
number of samples. Fewer bins means the axis is coarser (each output bin averages a wider slice of the linear grid);
more bins than `fft_size/2+1` means bins are interpolated *between* real FFT bins — smooth, but with no information
that was not already there. The one case with no loss is the identity grid — Scale = Linear, `blend = 0`,
`Display Max >= Nyquist`, `Output Bins = fft_size/2+1` — where `isIdentity()` is true and the warp is a `memcpy`:
the output is the linear FFT grid itself, copied, with no interpolation step and no rounding, every bit the
transform produced. That is a setting rather than a mode on purpose: a `Raw Linear Bins` toggle used to force it,
and it was removed because it could only ever disagree with the four sliders that already describe it.

### Output sample rate

The CHOP reports **`output_sample_rate = output bins × me.time.rate`**: one output vector of `bins`
samples is produced every `1/me.time.rate` seconds, so at the defaults (16384 bins, 60 fps) that is
**983 040 samples/s**. `me.time.rate` is the timeline rate *where the node lives*
(`OP_TimeInfo::rate`), so inside a component with Component Time the component's rate is used, not
the root's; it is read every cook, so an FPS change shows up on the next frame.

Read this as the rate of the frames **concatenated**: it says how fast spectrum data leaves the
node, which is what you size a buffer, a ring, a GPU upload or a network send with. It is not a
property of the spectrum itself, so it carries **no bin-index-to-Hz information** — a 16384-bin
vector describing 0…Nyquist and a 16384-bin vector describing 0…10 kHz both report 983 040.
**To convert a bin index to Hz, use `hz_per_sample` or `output_spectrum_axis`, never the sample
rate.**

#### The frequency axis

The band the bins actually describe is tracked separately, in the standard "bin 0 is DC, the last
bin is Nyquist" form: **`axis rate = 2 × (top of the axis)`**.

| Configuration | Axis (44.1 kHz input) |
|---|---|
| Linear grid, identity (`Scale = Linear`, `blend = 0`, Display Max ≥ Nyquist, `Output Bins = fft_size/2+1`) | **44100 Hz** — bin `i` is at `i*44100/fft_size` Hz, the transform's own grid |
| Scale = Linear, `blend = 0`, Display Max ≥ Nyquist, any other `Output Bins` | **44100 Hz** — the band is unchanged, only how many bins describe it |
| Display Max = 10000, any `Output Bins`, any uniform scale | **20000 Hz** — the axis really stops at 10 kHz |
| Log / Mel / ERB / Bark / Chroma (non-uniform bins) | **2 × Display Max** — exact at both ends (DC/floor … fmax); no single spacing describes a non-uniform grid in between |

`hz_per_sample = axis rate / (2 * (bins - 1))` — for the identity grid that is
`44100 / (2 * fft_size/2) = sr_in/fft_size`, the transform's own resolution. It does not depend on
how many `Output Bins` you chose: resampling changes how a band is described, not how wide it is.

#### Which channel to read

| Info CHOP channel | Meaning |
|---|---|
| `output_sample_rate` | `bins × me.time.rate` — what the CHOP reports to TouchDesigner (983 040 at the defaults) |
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

Re-measured on the development machine — **i9-13900H** (6 core / 12 thread Raptor Lake, AVX2 + FMA, no AVX-512),
FFTW3 3.3.11 AVX2 with a `FFTW_MEASURE` plan from wisdom, same 300-iteration `fft_bench` invocation:

| Configuration | FFT+mag | warp | dB | ballistics | total / channel |
|---|---|---|---|---|---|
| Default TD config, measured plan from wisdom | 22.45 µs | 4.66 µs | – | – | **28.96 µs** |
| Everything on (dB + A-weighting + ballistics; EQ unchanged) | 23.57 µs | 4.74 µs | 3.53 µs | 1.29 µs | **39.45 µs** |

Two honest notes on that block. First, `+ EQ Enable` re-measured at **0.01 µs**, i.e. it does not reproduce the
historical "+4 µs EQ" row on this machine — treat the EQ as free at the default shelf settings until a run says
otherwise. Second, the historical default row is 44–51 µs against this machine's 28.96 µs, but the two ran on
different builds, plans and CPUs, so that is **not** a speed-up claim; the controlled comparisons are the
library A/B below and the `--cook` Async numbers, which hold everything but the variable under test constant.

### Which FFT library is faster: FFTW3 vs Intel oneMKL

Same binary, same bench invocation, only the library differing (`--backend fftw3` vs `--backend mkl`), N = 16384,
16384 bins, i9-13900H:

| library | fft+mag | total per cook |
|---|---|---|
| FFTW3 3.3.11 AVX2, `FFTW_MEASURE` plan from wisdom | 11.94 µs | 21.06 µs |
| Intel oneMKL 2026.1.0 | 8.85 µs | 17.63 µs |

Four paired runs each way put oneMKL **13–34 % faster on the fft+mag stage** every time. That is the measured
reason the `FFT Backend` toggle exists; a re-run after a clean rebuild reproduced it (mkl 9.66 µs vs fftw3
14.77 µs on fft+mag, 4/4 pairs, −35 %).

**The A/B has to be interleaved, or it reports the opposite answer.** Each `fft_bench` process pays a cold
first-plan and cold-cache cost on whatever library it loads, so a single fftw3 run followed by a single mkl run
puts the *second* library ahead regardless of which is faster — measured here at 15.65 µs (fftw3, run second)
against 16.82 µs (mkl, run first), inverting the table above. Alternate the two backends within one loop and
re-run at least four pairs before believing any of it; the deployment notes, the OpenMP hazard it avoids and the list of DLLs
it needs are in [`3rdParty/fftw3/README.md`](PluginProjects/FFT/3rdParty/fftw3/README.md#using-intel-onemkl-instead-the-fft-backend-toggle).

The FFT is 75–85 % of the default cost; `Zero-Pad Len` is the lever that matters. History (same bench on every commit):
the July builds measured 47 µs (measured plan, EQ dead), `d60b7e3` turned the EQ on (+16 µs), `2daf9f1` switched to
ESTIMATE plans (+18 µs), `f1cb0d0`–`2daf9f1` had a scalar-log10 dB stage (+45 µs when dB was on).

### Threading

Two threads, both named for the debugger, **neither below normal priority**:

| Thread | Priority | Runs |
|---|---|---|
| analysis worker (`FFT Custom CHOP analysis`) | `THREAD_PRIORITY_HIGHEST` | window → FFT → magnitude → warp → weighting → dB → ballistics → peak |
| FFTW background planner (`FFT background planner`) | `THREAD_PRIORITY_ABOVE_NORMAL` | one `FFTW_MEASURE`/`FFTW_PATIENT` plan per FFT size, then exits |

The worker sits one notch above the planner so a cook always wins the core back from it; the planner is above
normal so a ~3 s `FFTW_PATIENT` measurement finishes in its budget under load instead of stretching out.

**FFTW's own threading does not help this node.** `fftwf_init_threads` / `fftwf_plan_with_nthreads` are exported by
the vendored `libfftw3f-3.dll`, but FFTW parallelizes the `howmany` loop and multi-dimensional transforms, *not* the
inside of a single 1-D transform — and `Mono Mix` (the default) issues exactly one transform per cook. Measured with
`fft_threads_probe.exe`: nthreads 1 / 2 / 4 / 6 → 68.8 / 85.3 / 81.7 / 102.5 µs, i.e. **slower** under both
`FFTW_ESTIMATE` and `FFTW_MEASURE` (−13 to −51 %). A `howmany = 4` plan does drop to 29.9 µs per transform, but that
is FFTW splitting the *batch*; the plugin gets the same effect without the plane of global FFTW state by running its
channel loop under `std::execution::par` (see below). Run the probe on your own machine before trusting the numbers.

**Where the threading actually is: not in this node.** This node is one mono channel per instance. Several
channels means several node instances, each with its own `FFT Custom CHOP analysis` worker — that is what spreads
across cores, and it needs no parameter. `Performance` has two controls: `Async`, and `FFT Backend` (which library,
not how many threads).

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
   "intermittent". Pulse **`Reloadplugin`** on the PluginBuilder COMP (`PluginBuilderExt.py:166`), or turn the
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
  cost 60.7 µs/cook and now costs 0.6 µs/cook. Whether TouchDesigner's own query was slow enough to be affected by
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
| Memoizing `statusSnapshot()` + `snapshotTail()` (v2.9.0) | **Kept, and measured.** 60.7 → 0.4 µs/cook (`fft_bench --info`). This is the only change in this table that was made against a number rather than against a correlation. |
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
