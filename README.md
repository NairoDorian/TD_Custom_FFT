# TouchDesigner Custom FFT Plugin (`Plugin_FFT`)

An ultra-high-performance, real-time Custom C++ CHOP (Channel Operator) plugin for **Derivative TouchDesigner**, featuring dual CPU FFT backends (**FFTW3** and **Intel MKL / IPP**), 256-bit AVX2 FMA SIMD vectorization, `FFTW_EXHAUSTIVE` plan benchmarking, and native TouchDesigner Python Textport console logging.

---

## ✨ Features

- **Dual CPU FFT Engine Selection**: Switch dynamically between `FFTW3` and `Intel MKL / IPP` from TouchDesigner's parameter drop-down menu.
- **`FFTW_EXHAUSTIVE` Plan Benchmarking**: Exhaustively benchmarks assembly codelets, SIMD vector factorizations, and memory strides on host CPU during setup for theoretical peak execution speed.
- **256-Bit AVX2 FMA Vectorization**:
  - De-interleaved FMA complex magnitude spectrum calculation (`computeMagnitudeAVX2_FMA`).
  - 2x unrolled AVX2 parallel peak magnitude search (`_mm256_max_ps`).
  - Vectorized decibel conversion and output range clamping.
- **Direct TouchDesigner Textport Console Logging**:
  - Dynamically resolves `python311.dll` / `python3.dll`, locks the Python GIL (`PyGILState_Ensure`), and executes Python `sys.stdout.write(...)` to log plan creation timing directly to TouchDesigner's Python Textport window (`Alt + 1`).
- **TouchDesigner Diagnostic Integration**:
  - **Middle-Click Info Popup**: Displays active engine status, plan level, timing measurements, and recent plan event logs.
  - **Info DAT Export**: Exposes dynamic table DAT rows containing full diagnostic state and plan logs.

---

## 🛠️ Project Structure

The project features a clean, minimal 5-file C++ source architecture:

```text
PluginProjects/FFT/source/
├── DSPModules.h       <-- Core DSP algorithms, IFFTEngine, FFTWEngine, MKLEngine, and Python logger
├── FFT.h              <-- TouchDesigner CHOP operator class header
├── FFT.cpp            <-- TouchDesigner CHOP operator class implementation
├── Parameters.h       <-- UI parameter definitions and constants
└── Parameters.cpp     <-- TouchDesigner parameter registration
```

---

## 🏗️ Building & Installation

### Requirements
- **Windows 10/11 64-bit**
- **Visual Studio 2022 / 2026** (MSVC 19.51+)
- **CMake** (v3.20+) and **Ninja**
- **Derivative TouchDesigner** (v2023 / v2024 / v2026)

### Build Steps

Initialize Visual Studio Developer Command Prompt and build with Ninja:

```cmd
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd PluginProjects/FFT
cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The compiled `FFT.dll` (671 KB) is automatically deployed to:
- `__Plugins__/FFT/FFT.dll`

---

## 🎛️ Parameters

On the **Spectrum** custom parameter page:

| Parameter | Type | Description |
| :--- | :--- | :--- |
| **FFT Engine** | Menu | Select backend engine (`FFTW3` or `Intel MKL / IPP`). |
| **Scale** | Menu | Select frequency re-mapping scale (`Linear`, `Log`, `Mel`, `ERB`, `Bark`, `Chroma`, `Melog`). |
| **Zero-Pad Len** | Menu | Select zero-padded FFT transform length ($1024$ to $65536$). |
| **Window** | Menu | Tapering window (`Rectangular`, `Hann`, `Hamming`, `Blackman`, `Kaiser`, `Flattop`). |
| **Equal-Loudness** | Menu | Acoustic weighting curve (`Off`, `A-weighting`, `C-weighting`, `ITU-R 468`). |
| **Decibel Mode** | Menu | Output mode (`Off`, `Raw dB`, `Normalized 0-1 dB`). |
| **Attack / Release** | Float | Envelope smoothing ballistics coefficients. |

---

## 📜 License

Created for TouchDesigner Custom C++ CHOP Development.
