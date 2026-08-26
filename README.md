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

- **FFTW3 R2C engine** with a selectable planner policy (`Auto` / `Fast` / `Measured`) and **wisdom caching**
  (`%LOCALAPPDATA%\TD_Custom_FFT\fftwf_wisdom.txt`): measured plans are ~40 % faster than estimated ones and
  only cost time the first time a size is used on the machine.
- **AVX2 / FMA** everywhere it pays: windowing, magnitude (rsqrt + Newton step, 2.2e-7 rel. error),
  warp interpolation (`vgatherdps`), weighting, single-gather 2048-entry LUT `20·log10` (0.004 dB error), ballistics, peak search.
- **Psychoacoustic scales**: Logarithmic, Mel, ERB, Bark, Chroma, Linear, Mel+Log blend, with a `Warp Blend`
  slider and an identity (memcpy) bypass when the grid is exactly linear.
- **Equal-loudness weighting**: A (IEC 61672), C, ITU-R 468.
- **dB modes** with **dB Reference**: Frame Peak (legacy, 0 dB = loudest bin), 0 dBFS (absolute), Slow AGC.
- **Magnitude normalization**: Coherent Gain (legacy, `mean(window) = 1`) or Full Scale (sine amplitude 1 → 1.0).
- **Window length** in samples (legacy) or in **milliseconds** (sample-rate independent).
- **Ballistics** as per-frame coefficients (legacy) or in **milliseconds** (frame-rate independent, uses `OP_TimeInfo`).
- **Parallel channels**: channels are processed with `std::execution::par` from a configurable channel count.
- **Diagnostics**: Info CHOP (`cook_time_us`, `peak_freq_hz`, `parallel_active`, …), Info DAT (plan log, wisdom path,
  window resolution), middle-click popup, warning/error strings, AVX2 CPU guard (no illegal-instruction crash).
- Textport logging through `PySys_WriteStdout` (no Python script injection).

## Project layout

```text
PluginProjects/FFT/
├── CMakeLists.txt        <-- 15 lines: include(PluginBuilder_V2/cmake/TDPlugin.cmake) + td_add_plugin(...)
├── plugin.json           <-- manifest read by PluginBuilder (family, optype, deps)
├── source/
│   ├── DSPModules.h      <-- TouchDesigner-independent DSP (FIFO, EQ, window, warp, weighting, dB, ballistics, FFTW engine)
│   ├── FFT.h / FFT.cpp   <-- the CHOP operator (API 10 entry points, per-channel pipeline, telemetry)
│   └── Parameters.h/.cpp <-- typed parameter definitions (enum classes, single eval() per cook)
├── tests/dsp_tests.cpp   <-- headless golden-vector tests (292 checks)
├── bench/bench.cpp       <-- per-stage benchmark
└── 3rdParty/fftw3/       <-- vendored libfftw3f-3 (header, .def/.lib, runtime DLL)
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
```
`PLUGIN_BUILDER_DIR` defaults to the sibling `../../../PluginBuilder_V2`; pass `-DPLUGIN_BUILDER_DIR=` otherwise.
A standalone build deploys `FFT.dll` + `libfftw3f-3.dll` into `__Plugins__/FFT/` (rename-in-place).

Requirements: Windows 10/11 x64, Visual Studio 2022/2026 C++ tools, CMake ≥ 3.21, Ninja, a CPU with AVX2 + FMA.

## Parameters

| Page | Parameter | Type | Default | Notes |
|---|---|---|---|---|
| Spectrum | Scale | Menu | Log | Log / Mel / ERB / Bark / Chroma / Linear / Melog |
| Spectrum | Display Max Hz | Float | 24000 | clamped to Nyquist |
| Spectrum | Output Bins | Int | 16384 | size of the warped output (hard-clamped 8…262144) |
| Spectrum | Warp Blend | Float | 0.963 | 0 = linear grid, 1 = fully perceptual |
| Spectrum | Log Floor Hz | Float | 20 | lowest frequency of the Log / Melog grid |
| Spectrum | Window Length Mode | Menu | Samples | Samples (legacy) or Milliseconds |
| Spectrum | Window Sampling | Int | 3175 | analysis window in samples (= 72 ms @ 44.1 kHz) |
| Spectrum | Window Length ms | Float | 72 | used when mode = Milliseconds |
| Spectrum | Zero-Pad Len | Menu | 32768 | FFT size (auto-grown to ≥ next pow2 of the window) |
| Spectrum | FFT Planner | Menu | Auto | Auto: instant plan now, measured plan upgraded in the background (wisdom-cached) · Fast (Estimate only) · Measured (blocking, once per size) |
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
| Performance | Parallel Channels | Toggle | Off | multithread channels (opt-in; worthwhile from ~8 channels) |
| Performance | Parallel Min Channels | Int | 8 | threshold for parallel processing |

Defaults (Coherent Gain, Frame Peak, Samples, Coefficient; EQ and Ballistics **off**) reproduce the spectrum of the
early builds, in which the EQ was inactive. Every optional section is bypassed entirely — code *and* parameter
reads — when disabled: 19 `getPar*` calls per cook in the default configuration instead of 31.

## Performance (fft_bench, i7-class desktop, 1 channel, N = 32768, 16384 bins, Log)

| Configuration | FFT+mag | warp | EQ | dB | total / channel |
|---|---|---|---|---|---|
| **Default TD config** (Loudness/Weighting/EQ/Ballistics off), cold plan | 41–48 µs | 4.8 µs | – | – | **44–51 µs** |
| Same after the background measured plan is in (or wisdom cached) | ~36 µs | 4.8 µs | – | – | **~42 µs** |
| + EQ Enable (6 dB high shelf, applied at ingest) | | | 3.6 µs | | +4 µs |
| Everything on (dB, A-weighting, ballistics, EQ) | 42–50 µs | 5 µs | 3.6 µs | 3.5 µs | **~60–70 µs** |
| N = 8192 | 6.4 µs | 4.7 µs | – | – | **~15 µs** |

The FFT is 75–85 % of the default cost; `Zero-Pad Len` is the lever that matters. History (same bench on every commit):
the July builds measured 47 µs (measured plan, EQ dead), `d60b7e3` turned the EQ on (+16 µs), `2daf9f1` switched to
ESTIMATE plans (+18 µs), `f1cb0d0`–`2daf9f1` had a scalar-log10 dB stage (+45 µs when dB was on).

## License / third party

FFTW3 is GPL. `FFT.dll` links `libfftw3f-3.dll`; distributing the plugin therefore falls under the GPL unless the
FFT backend is swapped (the `IFFTEngine` interface exists for that purpose — e.g. pffft/KissFFT (BSD) or oneMKL).
