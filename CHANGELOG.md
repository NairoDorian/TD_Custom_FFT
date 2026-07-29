# Changelog

All notable changes, architectural milestones, performance findings, and discoveries for `Plugin_FFT` are documented in this file.

---

## [v2.0.0] - 2026-07-29

### 🚀 Highlights & Breakthrough Findings

#### 1. Discovery: Direct C++ to TouchDesigner Textport Console Logging via Embedded CPython
- **Problem**: Standard C `printf` and `fflush(stdout)` from custom C++ OP DLLs write to Windows OS `stdout` (fd 1), which is not captured by TouchDesigner's Python Textport window (`Alt + 1`).
- **Solution**: Implemented a zero-dependency C++ bridge that dynamically resolves `python311.dll` / `python3.dll` at runtime, acquires the Python Global Interpreter Lock (GIL) via `PyGILState_Ensure()`, and executes Python's `sys.stdout.write(...)` via `PyRun_SimpleString()`.
- **Result**: C++ DSP log messages and benchmark timing now output directly into TouchDesigner's Python Textport console in real time!

#### 2. Real-Time `FFTW_PATIENT` Plan Benchmarking
- **Optimization**: Set `FFTW_PATIENT` as the primary planner flag, with `FFTW_MEASURE` and `FFTW_ESTIMATE` as fallbacks.
- **Result**: Generates highly optimized CPU SIMD execution plans instantly without long setup delays, maintaining sub-millisecond real-time cook loops.

---

### 🌟 Key Architectural Features

- **Consolidated 5-File Source Layout**:
  Consolidated all DSP modules, interfaces, and FFT engines into a minimal architecture:
  `DSPModules.h`, `FFT.h`, `FFT.cpp`, `Parameters.h`, `Parameters.cpp`.

- **Dual CPU FFT Backend**:
  - **FFTW3**: Single-precision 1D R2C transform with `FFTW_PATIENT` primary planning.
  - **Intel MKL / IPP**: High-performance Intel oneMKL / IPP execution.

- **AVX2 256-Bit FMA SIMD Magnitude Kernel**:
  Replaced horizontal additions (`_mm256_hadd_ps`) with 256-bit FMA SIMD de-interleaving (`computeMagnitudeAVX2_FMA`), evaluating 8 complex magnitude bins simultaneously without CPU pipeline stalls.

- **Vectorized Post-Processing**:
  - AVX2 2x unrolled peak magnitude search (`_mm256_max_ps`).
  - Vectorized decibel range conversion and output clamping (`_mm256_fmadd_ps`, `_mm256_max_ps`, `_mm256_min_ps`).
  - Zero-copy ballistics filter bypass when attack/release are set to `0.0`.

- **TouchDesigner Diagnostic Integration**:
  - **Middle-Click Info Popup**: Middle-clicking the `FFT` CHOP node in TouchDesigner displays active engine status, plan level, timing measurements, and the last 5 plan event logs.
  - **Info DAT Export**: Dynamic table DAT rows exporting active engine status and full plan event history.
